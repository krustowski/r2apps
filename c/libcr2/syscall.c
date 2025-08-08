#include "syscall.h"

int64_t syscall(int64_t number, int64_t arg1, int64_t arg2, int64_t arg3) 
{
	int64_t ret;
	asm volatile (
			"int $0x7f"
			: "=a"(ret)
			: "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
			: "rcx", "r11", "memory"
		     );
	return ret;
}

void exit(int64_t pid, int64_t code)
{
	syscall(ScExit, pid, code, 0);

	for (;;)
	{}
}

int64_t read_sysinfo(SysInfo_T *sysinfo)
{
	if (syscall(ScSysInfo, 0x01, (int64_t)sysinfo, 0))
	{
		return 0;
	}

	return 1;
}

int64_t write_sysinfo(const SysInfo_T *sysinfo)
{
	if (syscall(ScSysInfo, 0x02, (int64_t)sysinfo, 0))
	{
		return 0;
	}

	return 1;
}

int64_t print(const uint8_t *str) 
{
	int64_t len = 0;
	while (str[len]) ++len;

	if (len > 0)
	{
		if (syscall(ScPrints, (int64_t)str, len, 0))
		{
			return 0;
		}
	}

	return len;
}

int64_t read_file(const uint8_t *name, uint8_t *buffer)
{
	int64_t len = 0;
	while (name[len]) ++len;

	if (len > 0)
	{
		if (syscall(ScReadFile, (int64_t)name, (int64_t)buffer, 0))
		{
			return 0;
		}

		return 1;
	}

	return 0;
}

int64_t write_file(const uint8_t *name, const uint8_t *buffer)
{
	int64_t len = 0;
	while (name[len]) ++len;

	if (len > 0)
	{
		if (syscall(ScWriteFile, (int64_t)name, (int64_t)buffer, 0))
		{
			return 0;
		}

		return 1;
	}

	return 0;
}

int64_t rename_file(const uint8_t *old_name, const uint8_t *new_name)
{
	uint8_t len_old = 0, len_new = 0;
	while (old_name[len_old]) ++len_old;
	while (new_name[len_new]) ++len_new;

	if (len_old > 0 && len_new > 0)
	{
		if (syscall(ScRenameFile, (int64_t)old_name, (int64_t)new_name, 0))
		{
			return 0;
		}

		return 1;
	}

	return 0;
}

int64_t delete_file(const uint8_t *name)
{
	uint8_t len = 0;
	while (name[len]) ++len;

	if (len > 0)
	{
		if (syscall(ScDeleteFile, (int64_t)name, 0, 0))
		{
			return 0;
		}

		return 1;
	}

	return 0;
}

int64_t write_subdir(uint16_t cluster, const uint8_t *name)
{
	uint8_t len = 0;
	while (name[len]) ++len;

	if (len > 0)
	{
		if (syscall(ScWriteSubdir, (int64_t)cluster, (int64_t)name, 0))
		{
			return 0;
		}

		return 1;
	}

	return 0;
}

int64_t list_dir(int64_t cluster, Entry_T entries[32])
{
	if (syscall(ScListDir, cluster, (int64_t)entries, 0))
	{
		return 0;
	}

	return 1;
}

int64_t run_elf(const uint8_t *name, uint8_t *pid)
{
	if (syscall(ScRunELF, (int64_t)name, (int64_t)pid, 0))
	{
		return 0;
	}

	return 1;
}

int64_t read_port(uint8_t port, uint64_t *value)
{
	if (syscall(ScReadPort, (int64_t)port, (int64_t)value, 0))
	{
		return 0;
	}

	return 1;
}

int64_t write_port(uint8_t port, const uint64_t value)
{
	if (syscall(ScWritePort, (int64_t)port, (int64_t)value, 0))
	{
		return 0;
	}

	return 1;
}

int64_t serial_init()
{
	if (syscall(ScSerialPort, 0x01, 0x00, 0))
	{
		return 0;
	}

	return 1;
}

int64_t serial_read(uint32_t *value)
{
	if (syscall(ScSerialPort, 0x02, (int64_t)value, 0))
	{
		return 0;
	}

	return 1;
}

int64_t serial_write(const uint32_t value)
{
	if (syscall(ScSerialPort, 0x03, (int64_t)value, 0))
	{
		return 0;
	}

	return 1;
}

int64_t new_packet(uint8_t type, uint8_t *buffer) 
{
	if (syscall(ScNewPacket, (int64_t)type, (int64_t)buffer, 0))
	{
		return 0;
	}

	return 1;
}

int64_t send_packet(uint8_t type, uint8_t *buffer)
{
	if (syscall(ScSendPacket, (int64_t)type, (int64_t)buffer, 0))
	{
		return 0;
	}

	return 1;
}

