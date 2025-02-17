// SPDX-License-Identifier: BSD-2-Clause
// author: Max Kellermann <max.kellermann@gmail.com>

#ifndef DELETE_DISPOSER_HXX
#define DELETE_DISPOSER_HXX

/**
 * A disposer for boost::intrusive that invokes the "delete" operator
 * on the given pointer.
 */
class DeleteDisposer {
public:
	template<typename T>
	void operator()(T *t) {
		delete t;
	}
};

#endif
