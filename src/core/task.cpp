#include "task.h"

Task::Task(Task&& other) noexcept
{
	*this = std::forward<Task>(other);
}

Task::~Task()
{
	if (myDeleteFcnPtr)
		myDeleteFcnPtr(myCallableMemory.data(), myArgsMemory.data());
}

Task& Task::operator=(Task&& other) noexcept
{
	if (myDeleteFcnPtr)
		myDeleteFcnPtr(myCallableMemory.data(), myArgsMemory.data());

	myState = std::exchange(other.myState, {});
	myInvokeFcnPtr = std::exchange(other.myInvokeFcnPtr, nullptr);
	myCopyFcnPtr = std::exchange(other.myCopyFcnPtr, nullptr);
	myDeleteFcnPtr = std::exchange(other.myDeleteFcnPtr, nullptr);

	if (myCopyFcnPtr)
		myCopyFcnPtr(
			myCallableMemory.data(),
			other.myCallableMemory.data(),
			myArgsMemory.data(),
			other.myArgsMemory.data());

	if (myDeleteFcnPtr)
		myDeleteFcnPtr(other.myCallableMemory.data(), other.myArgsMemory.data());

	return *this;
}

Task::operator bool() const noexcept
{
	return myInvokeFcnPtr && myCopyFcnPtr && myDeleteFcnPtr && myState;
}

bool Task::operator!() const noexcept
{
	return !static_cast<bool>(*this);
}
