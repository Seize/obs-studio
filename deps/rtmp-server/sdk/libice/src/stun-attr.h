#ifndef _stun_attr_h_
#define _stun_attr_h_

#include <stdint.h>
#include "sys/sock.h"

struct stun_attr_t
{
	uint16_t type;
	uint16_t length;
	union
	{
		uint8_t					u8;
		uint16_t				u16;
		uint32_t				u32;
		uint64_t				u64;
		void*					data;
		char*					string;
		struct sockaddr_storage addr; // MAPPED-ADDRESS/XOR-MAPPED-ADDRESS
		struct {
			uint32_t			code;
			char*				reason_phrase;
		} errcode;
	} v;
};

int stun_attr_read(const uint8_t* data, const uint8_t* end, struct stun_attr_t *attrs, int n);

uint8_t* stun_attr_write(uint8_t* data, const uint8_t* end, const struct stun_attr_t *attrs, int n);

#endif /* !_stun_attr_h_ */
