/*
===========================================================================
Another Chance adaptation of DOOM 3 BFG Edition's intrusive idQueue.
Original source: neo/idlib/containers/Queue.h
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Adapted for standalone C++20 (nullptr, omitted idTech allocator test).
Licensed under GPL-3.0-or-later with DOOM 3 BFG additional terms.
See COPYING.txt.
===========================================================================
*/
#pragma once

// This is an intrusive queue: caller owns the queued objects and their lifetime.
template<typename T>
class idQueueNode {
public:
    idQueueNode() = default;
    T* GetNext() const { return next_; }
    void SetNext(T* value) { next_ = value; }

private:
    T* next_ = nullptr;
};

template<typename T, idQueueNode<T> T::* nodePtr>
class idQueue {
public:
    void Add(T* element) {
        (element->*nodePtr).SetNext(nullptr);
        if (last_) {
            (last_->*nodePtr).SetNext(element);
        } else {
            first_ = element;
        }
        last_ = element;
    }

    T* RemoveFirst() {
        T* element = first_;
        if (element) {
            first_ = (first_->*nodePtr).GetNext();
            if (last_ == element) {
                last_ = nullptr;
            }
            (element->*nodePtr).SetNext(nullptr);
        }
        return element;
    }

    T* Peek() const { return first_; }
    bool IsEmpty() const { return first_ == nullptr; }

private:
    T* first_ = nullptr;
    T* last_ = nullptr;
};
