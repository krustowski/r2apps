#include "mem.h"
#include "net.h"
#include "print.h"
#include "syscall.h"

/*
 *  garn
 *
 *  Sample HTTP server implementation.
 *
 *  krusty@vxn.dev / Aug 5, 2025
 */

//
//
//

int main(int64_t pid, int64_t arg)
{
	TcpSocket *server = socket_tcp4();
	bind(server, 80);
	listen(server);

	for (;;)
	{
		TcpSocket *client = accept(server);

		if (!client)
		{
			continue;
		}

		uint8_t buf[1024];
		uint32_t n = read(client, buf, sizeof(buf));

		if (n > 0 && memcmp(buf, "GET /", 6) == 0)
		{
			const uint8_t *response = 
				"HTTP/1.0 200 OK\r\n"
				"Content-Length: 13\r\n"
				"\r\n"
				"Hello, world!";
			write(client, (const uint8_t *) response, strlen(response));
			close(client);
		}
	}

	exit(pid, 456);
}
