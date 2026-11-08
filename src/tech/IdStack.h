/*
===========================================================================
Another Chance adaptation of DOOM 3 BFG Edition's intrusive idStack.
Original source: neo/idlib/containers/Stack.h
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Adapted for standalone C++20 and typed node links.
Licensed under GPL-3.0-or-later with DOOM 3 BFG additional terms.
See COPYING.txt.
===========================================================================
*/
#pragma once

// Intrusive stack; the stack never takes ownership of its nodes.
template<typename T>
class idStackNode {
public:
    idStackNode() = default;
    T* GetNext() const { return next_; }
    void SetNext(T* value) { next_ = value; }
private:
    T* next_ = nullptr;
};

template<typename T, idStackNode<T> T::* nodePtr>
class idStack {
public:
    void Add(T* element) {
        (element->*nodePtr).SetNext(top_);
        top_ = element;
        if (!bottom_) bottom_ = element;
    }

    T* Get() {
        T* element = top_;
        if (element) {
            top_ = (element->*nodePtr).GetNext();
            if (bottom_ == element) bottom_ = nullptr;
            (element->*nodePtr).SetNext(nullptr);
        }
        return element;
    }

    T* Peek() const { return top_; }
    bool IsEmpty() const { return top_ == nullptr; }

private:
    T* top_ = nullptr;
    T* bottom_ = nullptr;
};
