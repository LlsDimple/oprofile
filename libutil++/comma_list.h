/**
 * @file comma_list.h
 * Container holding items from a list of comma separated items
 *
 * @remark Copyright 2003 OProfile authors
 * @remark Read the file COPYING
 *
 * @author Philippe Elie
 */

#ifndef COMMA_LIST_H
#define COMMA_LIST_H

#include <string>
#include <vector>


#include "generic_spec.h"


/**
 * hold a list of item of type T, tracking also if item has been set.
 */
template <class T>
class comma_list
{
public:
	comma_list();

	/**
	 * @param str  list of comma separated item
	 * @param cumulative  must be true if item are cumulated through
	 * multiple call to this function else item are overwritten
	 *
	 * setup items array according to str parameters. Implement PP:3.17
	 * w/o restriction on charset and with the special string all which
	 * match anything.
	 */
	void set(std::string const & str, bool cumulative);

	/**
	 * @param value  the value to test
	 *
	 * return true if value match one the stored value in items
	 */
	bool match(T value) const;

	/**
	 * @param value  the value to test
	 *
	 * return true if value.is_all == true or value match one of the
	 * stored values in items
	 */
	bool match(generic_spec<T> const & value) const;

private:
	typedef T value_type;
	typedef std::vector<value_type> container_type;
	typedef typename container_type::const_iterator const_iterator;
	bool is_all;
	bool set_p;
	container_type items;
};


template <class T>
comma_list<T>::comma_list()
	:
	is_all(true),
	set_p(false)
{
}


template <class T>
void comma_list<T>::set(std::string const & str, bool cumulative)
{
	if (!cumulative)
		items.clear();

	// if we already set items and we cumulate item and we seen an "all"
	// item we must bailout to avoid to overwrite is_all member
	if (set_p && cumulative && is_all) {
		return;
	}

	is_all = false;
	set_p = true;

	std::vector<std::string> result;
	separate_token(result, str, ',');
	for (size_t i = 0 ; i < result.size() ; ++i) {
		if (result[i] == "all") {
			is_all = true;
			items.clear();
			break;
		}
		items.push_back(lexical_cast_no_ws<T>(result[i]));
	}
}


template <class T>
bool comma_list<T>::match(generic_spec<T> const & value) const
{
	if (is_all)
		return true;

	const_iterator cit = items.begin();
	const_iterator const end = items.end();

	for (; cit != end; ++cit) {
		if (value.match(*cit))
			return true;
	}

	return false;
}


#endif /* !COMMA_LIST_H */