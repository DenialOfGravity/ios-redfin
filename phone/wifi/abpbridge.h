#ifndef ABPBRIDGE_H
#define ABPBRIDGE_H
/* abpbridge: wland's end of kernel 1061's /dev/abpbridge (Apple CommCenter's QMUX pipes qmux1/qmux2). Off unless
 * abpbridge=1 is in /var/local/cell.on; P1: CTL answered here, every other request gets a QMI error (abperr=N). */
void abpbridge_poll(void);		/* from wland's idle loop */
#endif
