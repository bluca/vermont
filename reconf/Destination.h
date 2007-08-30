#ifndef PETERSINK_H
#define PETERSINK_H

/**
	@author Peter Baumann <siprbaum@users.berlios.de>
*/

#include "reconf/Emitable.h"

#include <cstdio>
#include <stdexcept>

class BaseDestination {
public:
	virtual ~BaseDestination();
	virtual void receive(Emitable *);
};

template<class T>
class Destination : public virtual BaseDestination
{
public:
	virtual void receive(Emitable* e)
	{
		T* p = dynamic_cast<T*>(e);
		if (!e) {
			throw std::runtime_error("Unsupported Emitable type\n");
		}
		this->receive(p);
	}

	virtual void receive(T* e)
	{
		printf("Emitable received\n");
	}


private:
	inline void recvPacket(T* p) {
		printf("recv called\n");
	}
};

#endif
