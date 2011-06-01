/*
 *  block-cache.hpp - Basic block cache management
 *
 *  Kheperix (C) 2003-2005 Gwenole Beauchesne
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef BLOCK_CACHE_H
#define BLOCK_CACHE_H

#include "block-alloc.hpp"

#define GATHER_BLOCK_CACHE_STATISTICS 0

template< class block_info, template<class T> class block_allocator = slow_allocator >
class block_cache
{
private:
	static const uint32 HASH_BITS = 15;
	static const uint32 HASH_SIZE = 1 << HASH_BITS;
	static const uint32 HASH_MASK = HASH_SIZE - 1;

	struct entry
		: public block_info
	{
		entry *					next_same_cl;
		entry **				prev_same_cl_p;
		entry *					next;
		entry **				prev_p;
	};

	block_allocator<entry>				allocator;
	entry *						cache_tags[HASH_SIZE];
	entry *						active;
	entry *						dormant;

	uint32 cacheline(uintptr addr) const {
		return (addr >> 2) & HASH_MASK;
	}

#ifdef GATHER_BLOCK_CACHE_STATISTICS
	struct {
		uint32 searches;
		uint32 hits;
		uint32 misses;
		uint32 faults;
	} stats;
#endif

public:

	block_cache();
	~block_cache();

	void print_statistics();

	block_info *new_blockinfo();
	void delete_blockinfo(block_info *bi);

	void initialize();
	void clear();
	void clear_range(uintptr start, uintptr end);
	block_info *fast_find(uintptr pc);
	block_info *find(uintptr pc);

	void remove_from_cl_list(block_info *bi);
	void remove_from_list(block_info *bi);
	void remove_from_lists(block_info *bi);

	void add_to_cl_list(block_info *bi);
	void raise_in_cl_list(block_info *bi);

	void add_to_active_list(block_info *bi);
	void add_to_dormant_list(block_info *bi);
};

template< class block_info, template<class T> class block_allocator >
block_cache< block_info, block_allocator >::block_cache()
	: active(NULL), dormant(NULL)
{
#ifdef GATHER_BLOCK_CACHE_STATISTICS
	memset(&stats, 0, sizeof(stats));
#endif
	initialize();
}

template< class block_info, template<class T> class block_allocator >
block_cache< block_info, block_allocator >::~block_cache()
{
	clear();
}

template< class block_info, template<class T> class block_allocator >
void block_cache< block_info, block_allocator >::initialize()
{
	for (int i = 0; i < HASH_SIZE; i++)
		cache_tags[i] = NULL;
}

template< class block_info, template<class T> class block_allocator >
void block_cache< block_info, block_allocator >::clear()
{
	entry *p;

	p = active;
	while (p) {
		entry *d = p;
		p = p->next;
		delete_blockinfo(d);
	}
	active = NULL;

	p = dormant;
	while (p) {
		entry *d = p;
		p = p->next;
		delete_blockinfo(d);
	}
	dormant = NULL;
}

template< class block_info, template<class T> class block_allocator >
void block_cache< block_info, block_allocator >::clear_range(uintptr start, uintptr end)
{
	if (!active)
		return;

	entry *p, *q;
	if (cacheline(start) < cacheline(end - 1)) {
		// Optimize for short ranges flush
		const int end_cl = cacheline(end - 1);
		for (int cl = cacheline(start); cl <= end_cl; cl++) {
			p = cache_tags[cl];
			while (p) {
				q = p;
				p = p->next_same_cl;
				if (q->intersect(start, end)) {
					q->invalidate();
					remove_from_cl_list(q);
					remove_from_list(q);
					delete_blockinfo(q);
				}
			}
		}
	}
	else {
		p = active;
		while (p) {
			q = p;
			p = p->next;
			if (q->intersect(start, end)) {
				q->invalidate();
				remove_from_cl_list(q);
				remove_from_list(q);
				delete_blockinfo(q);
			}
		}
	}
}

template< class block_info, template<class T> class block_allocator >
inline block_info *block_cache< block_info, block_allocator >::new_blockinfo()
{
	entry * bce = allocator.acquire();
	return bce;
}

template< class block_info, template<class T> class block_allocator >
inline void block_cache< block_info, block_allocator >::delete_blockinfo(block_info *bi)
{
	entry * bce = (entry *)bi;
	allocator.release(bce);
}

template< class block_info, template<class T> class block_allocator >
inline block_info *block_cache< block_info, block_allocator >::fast_find(uintptr pc)
{
	// Hit: return immediately (that covers more than 95% of the cases)
	entry * bce = cache_tags[cacheline(pc)];
	if (bce && bce->pc == pc)
		return bce;

	return NULL;
}

template< class block_info, template<class T> class block_allocator >
block_info *block_cache< block_info, block_allocator >::find(uintptr pc)
{
	const uint32 cl = cacheline(pc);

#ifdef GATHER_BLOCK_CACHE_STATISTICS
	stats.searches++;
#endif

	// Hit: return immediately
	entry * bce = cache_tags[cl];
	if (bce && bce->pc == pc) {
#ifdef GATHER_BLOCK_CACHE_STATISTICS
		stats.hits++;
#endif
		return bce;
	}

	// Miss: perform full list search and move block to front if found
	while (bce) {
		bce = bce->next_same_cl;
		if (bce && bce->pc == pc) {
			raise_in_cl_list(bce);
#ifdef GATHER_BLOCK_CACHE_STATISTICS
			stats.misses++;
#endif
			return bce;
		}
	}

	// Found none, will have to create a new block
#ifdef GATHER_BLOCK_CACHE_STATISTICS
	stats.faults++;
#endif
	return NULL;
}

template< class block_info, template<class T> class block_allocator >
void block_cache< block_info, block_allocator >::print_statistics()
{
#ifdef GATHER_BLOCK_CACHE_STATISTICS
	fprintf(stderr, "[Block Cache] Search Statistics: %9u searches, %9u hits, %9u misses, %9u faults\n",
		stats.searches, stats.hits, stats.misses, stats.faults);
	double hit_percent = (double)stats.hits / (double)stats.searches * 100.0,
		miss_percent = (double)stats.misses / (double)stats.searches * 100.0,
		fault_percent = (double)stats.faults / (double)stats.searches * 100.0;
	fprintf(stderr, "[Block Cache] In percentages: %3.2lf%% hits, %3.2lf%% misses, %3.2lf%% faults\n",
		hit_percent, miss_percent, fault_percent);
	memset(&stats, 0, sizeof(stats));
#endif
	uint32 c = 0, min = (uint32)-1, max = 0, average = 0;
	for(uint32 cl = 0; cl < HASH_SIZE; cl++) {
		if (cache_tags[cl] != NULL) {
			c++;
			entry *e = cache_tags[cl];
			uint32 ct = 0;
			while (e) {
				e = e->next_same_cl;
				ct++;
			}
			if (ct > max) max = ct;
			if (ct < min) min = ct;
			average += ct;
		}
	}
	average /= c;
	fprintf(stderr, "[Block Cache] %u of %u cache lines contain data\n",
		c, HASH_SIZE);
	fprintf(stderr, "[Block Cache] Line fill min: %u, max: %u, avg: %u\n",
		min, max, average);
}

template< class block_info, template<class T> class block_allocator >
void block_cache< block_info, block_allocator >::remove_from_cl_list(block_info *bi)
{
	entry * bce = (entry *)bi;
	if (bce->prev_same_cl_p)
		*bce->prev_same_cl_p = bce->next_same_cl;
	if (bce->next_same_cl)
		bce->next_same_cl->prev_same_cl_p = bce->prev_same_cl_p;
}

template< class block_info, template<class T> class block_allocator >
void block_cache< block_info, block_allocator >::add_to_cl_list(block_info *bi)
{
	entry * bce = (entry *)bi;
	const uint32 cl = cacheline(bi->pc);
	if (cache_tags[cl])
		cache_tags[cl]->prev_same_cl_p = &bce->next_same_cl;
	bce->next_same_cl = cache_tags[cl];
	
	cache_tags[cl] = bce;
	bce->prev_same_cl_p = &cache_tags[cl];
}

template< class block_info, template<class T> class block_allocator >
inline void block_cache< block_info, block_allocator >::raise_in_cl_list(block_info *bi)
{
	remove_from_cl_list(bi);
	add_to_cl_list(bi);
}

template< class block_info, template<class T> class block_allocator >
void block_cache< block_info, block_allocator >::remove_from_list(block_info *bi)
{
	entry * bce = (entry *)bi;
	if (bce->prev_p)
		*bce->prev_p = bce->next;
	if (bce->next)
		bce->next->prev_p = bce->prev_p;
}

template< class block_info, template<class T> class block_allocator >
void block_cache< block_info, block_allocator >::add_to_active_list(block_info *bi)
{
	entry * bce = (entry *)bi;
	
	if (active)
		active->prev_p = &bce->next;
	bce->next = active;
	
	active = bce;
	bce->prev_p = &active;
}

template< class block_info, template<class T> class block_allocator >
void block_cache< block_info, block_allocator >::add_to_dormant_list(block_info *bi)
{
	entry * bce = (entry *)bi;
	
	if (dormant)
		dormant->prev_p = &bce->next;
	bce->next = dormant;
	
	dormant = bce;
	bce->prev_p = &dormant;
}

template< class block_info, template<class T> class block_allocator >
inline void block_cache< block_info, block_allocator >::remove_from_lists(block_info *bi)
{
	remove_from_cl_list(bi);
	remove_from_list(bi);
}

#endif /* BLOCK_CACHE_H */
