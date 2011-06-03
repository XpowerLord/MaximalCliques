#ifndef _CLUSTERING_COMPONENTS_
#define _CLUSTERING_COMPONENTS_

#include <vector>
#include <list>
#include <cstdlib>
#include <cassert>

namespace clustering {

class components {
	int32_t N;
	std :: vector< std :: list <int32_t> > members; // this will include a lot of empty lists
	std :: vector< std :: list <int32_t> :: iterator > my_iterator; // for each node, the iterator to its position in all_members.
	std :: vector< int32_t                           > my_component; // for each node, the id of the cluster it is in.
public:
	components() : N(0) {}
	void setN(const int32_t N);
	void merge_components(const int32_t cl1, const int32_t cl2); // merge two non-empty components
	int32_t my_component_id(const int32_t n) const {
		assert(n>=0 && n<N);
		return this -> my_component.at(n);
	}
	const std :: list <int32_t> & get_members(int32_t component_id) const {
		assert(component_id >= 0 && component_id < this->N);
		return this->members.at(component_id);
	}
};
	
} // namespace clustering
#endif

