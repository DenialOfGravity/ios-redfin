/*
 * vtstub: hardware video decode stage 4 probe (docs/features/video.md). Installed as the flat Mach-O file
 * /System/Library/VideoDecoders/ExampleIPB.videodecoder (tools/sunfish-vtstub.sh), a slot VideoToolbox's
 * _VTLoadVideoDecoders knows: it dlopens the file (RTLD_LOCAL) and calls ExampleIPBDecoderRegister with no
 * meaningful args (research/notes/venus-stage1-2026-11-02.md section 7).
 * Version 1 registers nothing: it appends "pid NAME time" to /var/local/vtstub.log, so the log shows which
 * processes load VideoToolbox's decoder list. No constructor; errors are ignored (safe in any process).
 * Off: tools/sunfish-vtstub.sh remove.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

__attribute__((visibility("default"))) void ExampleIPBDecoderRegister(void)
{
	char line[128];
	int fd, n;

	fd = open("/var/local/vtstub.log", O_WRONLY | O_APPEND | O_CREAT, 0666);
	if (fd < 0)
		return;
	n = snprintf(line, sizeof(line), "%d %s %ld\n", getpid(), getprogname(), (long)time(NULL));
	if (n > 0)
		(void)write(fd, line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
	close(fd);
}
