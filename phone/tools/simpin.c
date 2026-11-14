/* simpin: SIM PIN client for wland's PIN socket (cell.c, pin=1), for unattended test cycles.
 *   simpin            -> status line ("pin app=... pin1=... retries=N ...")
 *   simpin PIN        -> one UIM Verify PIN through wland; prints wland's answer ("ok retries=N" / "err=..." / "refused ...")
 * The PIN is passed on the command line by the user's choice; this program keeps it in memory only. Claude 2026-11-09. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char **argv)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	char req[16], rep[128];
	int fd = socket(AF_UNIX, SOCK_STREAM, 0), n;
	size_t len = argc > 1 ? strlen(argv[1]) : 0;

	snprintf(a.sun_path, sizeof(a.sun_path), "%s", "/var/run/wland-cell.sock");
	if (fd < 0 || connect(fd, (struct sockaddr *) &a, sizeof(a))) {
		perror("simpin: connect");
		return 1;
	}
	if (len) {
		if (len < 4 || len > 8) {
			fprintf(stderr, "simpin: PIN must be 4-8 digits\n");
			return 1;
		}
		req[0] = 'V';
		req[1] = (char) len;
		memcpy(req + 2, argv[1], len);
		n = (int) write(fd, req, 2 + len);
	} else
		n = (int) write(fd, "S\n", 2);
	if (n <= 0) {
		perror("simpin: write");
		return 1;
	}
	for (int t = 0; t < 100; t++) {		/* wland answers within 8 s (PIN_REPLY_MS) */
		n = (int) read(fd, rep, sizeof(rep) - 1);
		if (n > 0) {
			rep[n] = 0;
			fputs(rep, stdout);
			return strncmp(rep, "ok", 2) == 0 || rep[0] == 'p' ? 0 : 2;
		}
		if (n == 0)
			break;
		usleep(100000);
	}
	fprintf(stderr, "simpin: no answer\n");
	return 3;
}
