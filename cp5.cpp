#include "graph/network.hpp"
#include "graph/loading.hpp"
#include "graph/stats.hpp"
#include "clustering/components.hpp"
#include "macros.hpp"
#include "cliques.hpp"
#include "cmdline.h"

#include <getopt.h>
#include <libgen.h>
#include <ctime>
#include <algorithm>
#include <vector>
#include <tr1/functional>
#include <cassert>
#include <limits>
#include <iostream>
#include <sys/stat.h> // for mkdir
#include <sys/types.h> // for mkdir
#include <fstream>
#include <sstream>
#include <cerrno>
#include <tr1/unordered_set>


using namespace std;

typedef vector<int32_t> clique; // the nodes will be in increasing numerical order

static void do_clique_percolation_variant_5b(const int32_t min_k, const int32_t max_k, const vector< clique > &the_cliques, const char * output_dir_name, const graph :: NetworkInterfaceConvertedToString *network) ;
static void write_all_communities_for_this_k(const char * output_dir_name
		, const int32_t k
		, const vector<int32_t> &found_communities
		, const clustering :: components & current_percolation_level
		, const vector<clique> &the_cliques
		, const graph :: NetworkInterfaceConvertedToString *network
		);
static void create_directory_for_output(const char *dir);

template<typename T>
string thou(T number);
#define PPt(x) PP(thou(x))
#define ELAPSED (double(clock())/CLOCKS_PER_SEC)
#define HOWLONG "(runtime: " << ELAPSED <<"s)"

static string memory_usage() {
	ostringstream mem;
	PP("hi");
	ifstream proc("/proc/self/status");
	string s;
	while(getline(proc, s), !proc.fail()) {
		if(s.substr(0, 6) == "VmSize") {
			mem << s;
			return mem.str();
		}
	}
	return mem.str();
}

int main(int argc, char **argv) {
	gengetopt_args_info args_info;

	// there shouldn't be any errors in processing args
	if (cmdline_parser (argc, argv, &args_info) != 0)
		exit(1) ;
	// .. and there should be exactly one non-option arg
	if(args_info.inputs_num != 2 || args_info.k_arg < 3) {
		cmdline_parser_print_help();
		cerr << endl << " == Need two args: edge_list and output_dir ==" << endl;
		exit(1);
	}

	const char * edgeListFileName   = args_info.inputs[0];
	const char * output_dir_name   = args_info.inputs[1];
	const int min_k = args_info.k_arg;

        std :: auto_ptr<graph :: NetworkInterfaceConvertedToString > network;
	if(args_info.stringIDs_flag) {
		network	= graph :: loading :: make_Network_from_edge_list_string(edgeListFileName, false, false, true);
	} else {
		network	= graph :: loading :: make_Network_from_edge_list_int64(edgeListFileName, false, false, true, 0);
	}

	PP(memory_usage());

	int32_t maxDegree = graph :: stats :: get_max_degree(network->get_plain_graph());

	cerr << "Network loaded"
	       << " after " << (double(clock()) / CLOCKS_PER_SEC) << " seconds. "
		<< network.get()->numNodes() << " nodes and " << network.get()->numRels() << " edges."
		<< " Max degree is " << maxDegree
	       << endl;

	vector< clique > the_cliques;
	cliques :: cliquesToVector(network.get(), min_k, the_cliques);

	const int32_t C = the_cliques.size();
	if(C==0) {
		cerr << endl << "Error: you don't have any cliques of at least size " << min_k << ". Exiting." << endl;
		exit(1);
	}
	int max_clique_size = 0; // to store the size of the biggest clique
	for(vector<clique > :: const_iterator i = the_cliques.begin(); i != the_cliques.end(); i++) {
		if(max_clique_size < (int)i->size())
			max_clique_size = i->size();
	}
	PP(max_clique_size);
	assert(max_clique_size > 0);

	// finally, call the clique_percolation algorithm proper

	do_clique_percolation_variant_5b(min_k, max_clique_size, the_cliques, output_dir_name, network.get());
}

class bloom { // http://en.wikipedia.org/wiki/Bloom_filter
	vector<bool> data;
public: // make private
	static const int64_t l;
	int64_t occupied;
	int64_t calls_to_set;
	std :: tr1 :: hash<int64_t> h;
public:
	bloom() : data(this->l), occupied(0), calls_to_set(0) {  // 10 giga-bits 1.25 GB
	}
	bool test(const int64_t a) const {
		const int64_t b = h(a) % l;
		return this->data.at(b);
	}
	void set(const int64_t a)  {
		++ this->calls_to_set;
		const int64_t b = h(a) % l;
		bool pre = this->data.at(b);
		this->data.at(b) = true;
		if(!pre) {
			++ this->occupied;
		}
	}
};
const int64_t bloom :: l = 10000000000;
class intersecting_clique_finder { // based on a tree of all cliques, using a bloom filter to cut branch from the search tree
	bloom bl;
public:
	const int32_t power_up; // the next power of two above the number of cliques
	intersecting_clique_finder(const int32_t p) : power_up(p) {
	}
	const bloom & get_bloom_filter(void) const { return this->bl; }
	int32_t overlap_estimate(const clique &new_clique, const int32_t branch_identifier) const {
		int32_t potential_overlap = 0;
		for(size_t n = 0; n < new_clique.size(); n++) {
			const int32_t node_id = new_clique.at(n);
			const int64_t a = (int64_t(branch_identifier) << 32) + node_id;
			potential_overlap += this->bl.test(a) ? 1 : 0;
		}
		return potential_overlap;
	}
	void add_clique_to_bloom(const clique &new_clique, int32_t branch_identifier) {
		while(branch_identifier) {
			for(size_t n = 0; n < new_clique.size(); n++) {
				const int32_t node_id = new_clique.at(n);
				const int64_t a = (int64_t(branch_identifier) << 32) + node_id;
				this->bl.set(a);
			}
			branch_identifier >>= 1;
		}
	}
};
struct too_many_cliques_exception : public std :: exception { };

struct assigned_branches_t {
	int32_t power_up;
	int32_t number_of_cliques;
	vector<bool> assigned_branches; // the branches where all subleaves have already been assigned. // the recursive search should stop immediately
	assigned_branches_t(int32_t p, int32_t C) : power_up(p), number_of_cliques(C) {
		assigned_branches.resize(2*power_up, false);
		int total_premarked_as_invalid = 0;
		for(int invalid_leaf = power_up + C; invalid_leaf < 2 * power_up; invalid_leaf++ ) {
			int marked_this_time = this->mark_as_done(invalid_leaf);
			total_premarked_as_invalid += marked_this_time;
		}
	}
	int32_t mark_as_done(const int32_t branch_id) {
		assert(branch_id >= 0 && size_t(branch_id) < this->assigned_branches.size());
		int32_t marked_this_time = 0;
		if(this->assigned_branches.at(branch_id) == false) {
			this->assigned_branches.at(branch_id) = true;
			++ marked_this_time;
			if(branch_id > 1) { // all branches, but the root, will have a partner
			                    // .. if the partner is marked, then move up
				if(this->assigned_branches.at(branch_id ^ 1) == true) {
					marked_this_time += this->mark_as_done(branch_id >> 1);
				}
			}
		}
		return marked_this_time;
	}
};

static int32_t actual_overlap(const clique &old_clique, const clique &new_clique) ;

static int64_t calls_to_recursive_search = 0;

static void recursive_search(const intersecting_clique_finder &search_tree
		, const int32_t branch_identifier
		, const int32_t current_clique_id
		, const int32_t t
		, const vector<clique> &the_cliques
		, int32_t &searches_performed
		, vector<int32_t> &cliques_found
		, const clustering :: components & current_percolation_level
		, const int32_t component_to_skip
		, assigned_branches_t &assigned_branches
		) {
	++ calls_to_recursive_search;
	assert(calls_to_recursive_search > 0);
	// PRECONDITION:
	//    - we assume that the current branch in the tree already has enough nodes
	//
	// is this a leaf node?
	//   - if at a leaf node
	//     - is it a valid clique (i.e. leaf_node_id < the_cliques.size()?
	//       - if so, ignore it
	//       - if not:
	//         - is it already in the community we're forming? if so, ignore it (but count it)
	//         - perform the validation. (actual_overlap)
	//   - if not at a leaf
	//     - decide which sub branches, if any, to visit. And decide in what order.

	assert(assigned_branches.assigned_branches.at(branch_identifier) == false);

	const clique &current_clique = the_cliques.at(current_clique_id);
	if(branch_identifier >= search_tree.power_up) {
		const int32_t leaf_clique_id = branch_identifier - search_tree.power_up;
		assert(leaf_clique_id >= 0); // remember, this leaf mightn't really represent a clique (i.e. leaf_clique_id >= the_cliques.size()) 
		if(size_t(leaf_clique_id) >= the_cliques.size()) {
			// PP2(leaf_clique_id, the_cliques.size()); // TODO: get out of branches earlier
		} else {
			const int32_t component_id_of_leaf = current_percolation_level.my_component_id(leaf_clique_id);
			if(component_id_of_leaf == component_to_skip) {
				// PP2(component_id_of_leaf, component_to_skip);
				// PP2(current_clique_id, leaf_clique_id);
				assert(component_id_of_leaf != component_to_skip);
			} else {
				// time to check if this clique really does have a big enough overlap

				assert(size_t(leaf_clique_id) < the_cliques.size());
				const int32_t actual = actual_overlap(the_cliques.at(leaf_clique_id), current_clique) ;
				assert(leaf_clique_id != current_clique_id);
				assert(actual < (int32_t)current_clique.size()); // don't allow it to match with itself, did you forget to include the seed into the community?
				if(actual >= t) {
					assert(leaf_clique_id >= 0 && size_t(leaf_clique_id) < the_cliques.size());
					cliques_found.push_back(leaf_clique_id);
					assigned_branches.mark_as_done(branch_identifier); // this is *critical* for speed (if not accuracy). it stops it checking frontier<>frontier links. Really should consider a DFS now!
				}
			}
		}
	} else {
		const int32_t left_subnode_id = branch_identifier << 1;
		assert(left_subnode_id >= 0);  // just in case the <<1 made it negative
		const int32_t right_subnode_id = left_subnode_id + 1;
#define branch_cut
#ifdef branch_cut
		const int32_t potential_overlap_left  =
			assigned_branches.assigned_branches.at(left_subnode_id) ? 0 :
			search_tree.overlap_estimate(current_clique, left_subnode_id);
		const int32_t potential_overlap_right =
			assigned_branches.assigned_branches.at(right_subnode_id) ? 0 :
			search_tree.overlap_estimate(current_clique, right_subnode_id);
#endif
		searches_performed += 2; // checked the left and the right
#ifdef branch_cut
		if(potential_overlap_left >= t)
#endif
			recursive_search(search_tree, left_subnode_id, current_clique_id, t, the_cliques, searches_performed, cliques_found, current_percolation_level, component_to_skip, assigned_branches);
#ifdef branch_cut
		if(potential_overlap_right >= t)
#endif
			recursive_search(search_tree, right_subnode_id, current_clique_id, t, the_cliques, searches_performed, cliques_found, current_percolation_level, component_to_skip, assigned_branches);
	}
}

static void neighbours_of_one_clique(const vector<clique> &the_cliques
		, const int32_t current_clique_id
		, const clustering :: components & components
		, const int32_t t
		, const int32_t current_component_id
		, const intersecting_clique_finder & search_tree
		, int32_t &searches_performed
		, vector<int32_t> &cliques_found
		, assigned_branches_t &assigned_branches
		) {
	// given:
	//    - one clique,
	//    - a component object describing all the components for the current value of k,
	//    - the intersection threshold (t)   { e.g. t = k-1 }
	//    - the component the clique is currently in (it's probably just been recently added)
	// return:
	//    - the list of cliques adjacent to the clique (at least t nodes in common),
	//    - BUT without the cliques that are already in the current component
		assert(current_component_id == components.my_component_id(current_clique_id));
		const int32_t root_node = 1;
		if(assigned_branches.assigned_branches.at(root_node) == false) // otherwise, we've assigned everything and the algorithm can complete
			recursive_search(search_tree
				, root_node
				, current_clique_id
				, t
				, the_cliques
				, searches_performed
				, cliques_found
				, components
				, current_component_id
				, assigned_branches
				);
}

static void one_k (vector<int32_t> & found_communities
		, vector<int32_t> candidate_components
		, clustering :: components &current_percolation_level
		, const int32_t t
		, const vector<clique> &the_cliques
		, const int32_t power_up
		, const int32_t C
		, const intersecting_clique_finder &isf
	     );

static void do_clique_percolation_variant_5b(const int32_t min_k, const int32_t max_k, const vector< clique > &the_cliques, const char * output_dir_name, const graph :: NetworkInterfaceConvertedToString *network) {

	assert(network);
	assert(output_dir_name);
	if(the_cliques.size() > static_cast<size_t>(std :: numeric_limits<int32_t> :: max())) {
		throw too_many_cliques_exception();
	}
	const int32_t C = the_cliques.size();
	if(C==1) { // nothing to be done, just one clique. Leave it on its own.
		return;
	}

	vector<clustering :: components> all_percolation_levels(max_k+1);
	for(int32_t i = min_k; i <= max_k; i++) {
		clustering :: components &one_percolation_level = all_percolation_levels.at(i);
		one_percolation_level.setN(C);
	}

	PP3(C, min_k, max_k);
	assert(min_k > 0 && min_k <= max_k && C > 1);

	int32_t power_up = 1; // this is to be the smallest power of 2 greater than, or equal to, the number of cliques
	while(power_up < C)
		power_up <<= 1;
	assert(power_up > 0); // make sure it hasn't looped around and become negative!
	PP2(C, power_up);

	/*
	 * The above is generic to all k
	 * The rest is specific for each k
	 */

	/* Here's the loop:
	 *
	 * - take list of source components as input for a new value of k
	 * - for each source component 
	 *   - do the comm-finding in it, appending the found communities to found_communities,
	 *                                 and recording the detail in aa components structure.
	 * - then prepare the next set of candidates, by copying the relevant components
	 *               and cliques into the next value of k
	 */
	clustering :: components & lowest_percolation_level = all_percolation_levels.at(min_k);
	const int32_t first_candidate_community = lowest_percolation_level.top_empty_component();
	for(int c=0; c<C; c++) {
		lowest_percolation_level.move_node(c,first_candidate_community); // move 'node' c (i.e. the c-th clique) into component 0
		// all the nodes (cliques) are in one big community for now.
	}
	vector<int32_t> source_components;
	source_components.push_back(first_candidate_community);

	create_directory_for_output(output_dir_name);
	for(int32_t k = min_k; k<=max_k; k++) {
		vector<int32_t> found_communities;
		PP2(k, ELAPSED);
		clustering :: components & current_percolation_level = all_percolation_levels.at(k);
		const int32_t t = k-1;

		intersecting_clique_finder isf(power_up);
		for(int c = 0; c < C; c++) {
			if(the_cliques.at(c).size() >= size_t(t))
				isf.add_clique_to_bloom(the_cliques.at(c), c+power_up);
		}
		cout << "isf populated for k = " << k << ". " << HOWLONG << endl;
		PPt(isf.get_bloom_filter().l);
		PPt(isf.get_bloom_filter().calls_to_set);
		PPt(isf.get_bloom_filter().occupied);

		one_k(
			found_communities
			, source_components
			, current_percolation_level
			, t
			, the_cliques
			, power_up
			, C
			, isf
			);
		cout << HOWLONG << endl;
		/* The found communities are now in found_communities
		 * Gotta write them out
		 */
		write_all_communities_for_this_k(output_dir_name, k, found_communities, current_percolation_level, the_cliques, network);
		cout << HOWLONG << endl;

		const int32_t new_k = k + 1;
		PP(new_k);
		if(new_k > max_k)
			break;
		clustering :: components & new_percolation_level = all_percolation_levels.at(new_k);

		source_components.clear();
		PP(__LINE__);
		for(int32_t f = 0; f < (int32_t) found_communities.size(); f++) {
			PP2(f, ELAPSED);
			const int32_t new_cand = new_percolation_level.top_empty_component();
			const clustering :: member_list_type & members_of_this_found_community = current_percolation_level.get_members(found_communities.at(f));
			int32_t number_of_big_enough_cliques = 0;
			for( clustering :: member_list_type :: const_iterator i = members_of_this_found_community.get().begin()
				; i != members_of_this_found_community.get().end()
				; i++) {
				const int32_t clique_id = *i;
				if(the_cliques.at(clique_id).size() >= size_t(new_k)) {
					new_percolation_level.move_node(clique_id, new_cand);
					++ number_of_big_enough_cliques;
				}
			}
			if(new_cand == new_percolation_level.top_empty_component()) {
			} else {
				source_components.push_back(new_cand);
			}
		}
		PP(__LINE__);
	}
}

static void one_k (vector<int32_t> & found_communities
		, vector<int32_t> candidate_components
		, clustering :: components &current_percolation_level
		, const int32_t t
		, const vector<clique> &the_cliques
		, const int32_t power_up
		, const int32_t C
		, const intersecting_clique_finder &isf
	     ) {
	/* We need a function that,
	 *  - given a list of source components, where the small cliques have been kept out
	 *  - does clique percolation on that, returning the component_ids of the found communities
	 */
	assert(found_communities.empty());

	assigned_branches_t assigned_branches(power_up, C); // the branches where all subleaves have already been assigned.  the recursive search should stop immediately upon reaching one of these
	{ // all the cliques that are too small should be premarked as assigned
		for(int c=0; c<C; c++) {
			const int32_t clique_size = the_cliques.at(c).size();
			if(clique_size <= t) {
				assigned_branches.mark_as_done(power_up + c);
			}
		}
	}


while (!candidate_components.empty()) {
	const int32_t source_component = candidate_components.back();
	PP2(t+1, source_component);
	cout << HOWLONG << endl;
	candidate_components.pop_back();
	while(!current_percolation_level.get_members(source_component).empty()) { // keep pulling out communities from current source-component
		// - find a clique that hasn't yet been assigned to a community
		// - create a new community by:
		//   - make it the first 'frontier' clique
		//   - keep adding it, and all its neighbours, to the community until the frontier is empty

		assert(!current_percolation_level.get_members(source_component).empty());
		const int32_t seed_clique = current_percolation_level.get_members(source_component).get().front();
		PP(seed_clique);
		assert(assigned_branches.assigned_branches.at(power_up + seed_clique) == false);
		assert(the_cliques.at(seed_clique).size() > size_t(t));

		stack< int32_t, vector<int32_t> > frontier_cliques;
		frontier_cliques.push(seed_clique);
		const int32_t component_to_grow_into = current_percolation_level.top_empty_component();
		assert(0 == current_percolation_level.get_members(component_to_grow_into).size());

		current_percolation_level.move_node(seed_clique, component_to_grow_into);

		while(!frontier_cliques.empty()) {
			// PP(frontier_cliques.size());
			const int32_t popped_clique = frontier_cliques.top();
			frontier_cliques.pop();

			assigned_branches.mark_as_done(power_up + popped_clique);

			int32_t searches_performed = 0;
			vector<int32_t> fresh_frontier_cliques_found;
			const int32_t current_component_id = current_percolation_level.my_component_id(popped_clique);
			assert(current_component_id == component_to_grow_into);
			neighbours_of_one_clique(the_cliques, popped_clique, current_percolation_level, t, component_to_grow_into, isf, searches_performed, fresh_frontier_cliques_found, assigned_branches);
			// int32_t search_successes = fresh_frontier_cliques_found.size();
			// const int32_t old_size_of_growing_community = current_percolation_level.get_members(component_to_grow_into).size();
			for(int x = 0; x < (int)fresh_frontier_cliques_found.size(); x++) {
				const int32_t frontier_clique_to_be_moved_in = fresh_frontier_cliques_found.at(x);
				frontier_cliques.push(frontier_clique_to_be_moved_in);
				assert(source_component == current_percolation_level.my_component_id(frontier_clique_to_be_moved_in));
				current_percolation_level.move_node(frontier_clique_to_be_moved_in, component_to_grow_into);
			}
			// const int32_t new_size_of_growing_community = current_percolation_level.get_members(component_to_grow_into).size();
			assert(frontier_cliques.size() < the_cliques.size());
		}
		const int32_t final_size_of_growing_community = current_percolation_level.get_members(component_to_grow_into).size();
		PP2(t+1, final_size_of_growing_community);
		// PP(calls_to_recursive_search);
		found_communities.push_back(component_to_grow_into);
	}
} // looping over the source components
}

template<typename T>
string thou(T number) {
	std::ostringstream ss;
	ss.imbue(std::locale(""));
	ss << number;
	return ss.str();
}


static int32_t actual_overlap(const clique &old_clique, const clique &new_clique) {
	vector<int32_t> intersection;
	set_intersection( old_clique.begin(), old_clique.end()
	                 ,new_clique.begin(), new_clique.end()
			 , back_inserter(intersection));
	return intersection.size();
}

static void create_directory_for_output(const char *dir) {
	assert(dir);
	{
		const int32_t ret = mkdir(dir, 0777);
		if(ret != 0 && errno != EEXIST) {
			cerr << endl << "Couldn't create directory \"" << dir << "\". Exiting." << endl;
			exit(1);
		}
	}
}
static void write_all_communities_for_this_k(const char * output_dir_name
		, const int32_t k
		, const vector<int32_t> &found_communities
		, const clustering :: components & current_percolation_level
		, const vector<clique> &the_cliques
		, const graph :: NetworkInterfaceConvertedToString *network
		) {
			assert(output_dir_name);
			ostringstream output_file_name;
			output_file_name << output_dir_name << "/" << "comm" << k;
			ofstream write_nodes_here(output_file_name.str().c_str());
			for(int32_t f = 0; f < (int32_t) found_communities.size(); f++) { // communities
				tr1 :: unordered_set<int32_t> node_ids_in_this_community;
				const clustering :: member_list_type & members_of_this_found_community = current_percolation_level.get_members(found_communities.at(f));
				for( clustering :: member_list_type :: const_iterator i = members_of_this_found_community.get().begin()
					; i != members_of_this_found_community.get().end()
					; i++) { // cliques
					const int32_t clique_id = *i;
					const clique & one_clique = the_cliques.at(clique_id);
					for(int i = 0; size_t(i)<one_clique.size(); i++) {
						node_ids_in_this_community.insert(one_clique.at(i));
					}
				} // cliques in the comm
				bool first_node_on_this_line = true;
				for(std :: tr1 :: unordered_set<int32_t> :: const_iterator it = node_ids_in_this_community.begin()
						; it != node_ids_in_this_community.end()
						; ++it ) {
					const int32_t node_id = *it;
					const std :: string node_name = network->node_name_as_string(node_id);
					if(!first_node_on_this_line)
						write_nodes_here << ' ';
					write_nodes_here << node_name;
					first_node_on_this_line = false;
				} // writing nodes
				write_nodes_here << endl;
			} // communities
			write_nodes_here.close();
}

