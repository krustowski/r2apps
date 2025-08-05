#include "syscall.h"

/*
 *  icmpresp
 *
 *  This program acts like a sample service for the rou2exOS kernel. It is to receive
 *  an ICMP pakcet, to parse it and to respond accordingally.
 *
 *  As the time of the initial development this prog could be run in userspace.
 *
 *  krusty@vxn,dev / Aug 5, 2025
 */

int main(int64_t pid, int64_t arg)
{
	print("-> icmpresp service start\n");

	for (;;)
	{
		// port re
	}

	print("*** Exit\n");
	exit(pid, 111);
}
