/*
* UAE - The Un*x Amiga Emulator
*
* Not a parser, but parallel and serial emulation for Win32
*
* Copyright 1997 Mathias Ortmann
* Copyright 1998-1999 Brian King - added MIDI output support
*/

#include "sysconfig.h"

#undef SERIAL_ENET

#include <windows.h>
#include <winspool.h>
#include <stdlib.h>
#include <stdarg.h>
#include <mmsystem.h>
#include <ddraw.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <io.h>

#include <setupapi.h>

#include "sysdeps.h"
#include "options.h"
#include "gensound.h"
#include "events.h"
#include "uae.h"
#include "include/memory.h"
#include "custom.h"
#include "autoconf.h"
#include "newcpu.h"
#include "traps.h"
#include "registry.h"
#include "od-win32/win32gui.h"
#include "od-win32/parser.h"
#include "od-win32/midi.h"
#include "od-win32/ahidsound.h"
#include "picasso96_win.h"
#include "win32gfx.h"
#include "win32.h"
#include "ioport.h"
#include "parallel.h"
#include "zfile.h"
#include "threaddep/thread.h"
#include "serial.h"
#include "savestate.h"
#include "ahidsound_new.h"
#include "uaeipc.h"
#include "xwin.h"

#include <Ghostscript/errors.h>
#include <Ghostscript/iapi.h>

#define MIN_PRTBYTES 10

static uae_char prtbuf[PRTBUFSIZE];
static int prtbufbytes,wantwrite;
static HANDLE hPrt = INVALID_HANDLE_VALUE;
static DWORD  dwJob;
static int prtopen;
extern void flushpixels(void);
void DoSomeWeirdPrintingStuff(uae_char val);
static int uartbreak;
static int parflush;

static volatile int prt_running;
static volatile int prt_started;
static smp_comm_pipe prt_requests;

int postscript_print_debugging = 0;
static struct zfile *prtdump;

static int psmode = 0;
static HMODULE gsdll;
static gs_main_instance *gsinstance;
static int gs_exitcode;

typedef int (CALLBACK* GSAPI_REVISION)(gsapi_revision_t *pr, int len);
static GSAPI_REVISION ptr_gsapi_revision;
typedef int (CALLBACK* GSAPI_NEW_INSTANCE)(gs_main_instance **pinstance, void *caller_handle);
static GSAPI_NEW_INSTANCE ptr_gsapi_new_instance;
typedef void (CALLBACK* GSAPI_DELETE_INSTANCE)(gs_main_instance *instance);
static GSAPI_DELETE_INSTANCE ptr_gsapi_delete_instance;
typedef int (CALLBACK* GSAPI_SET_STDIO)(gs_main_instance *instance,
	int (GSDLLCALLPTR stdin_fn)(void *caller_handle, char *buf, int len),
	int (GSDLLCALLPTR stdout_fn)(void *caller_handle, const char *str, int len),
	int (GSDLLCALLPTR stderr_fn)(void *caller_handle, const char *str, int len));
static GSAPI_SET_STDIO ptr_gsapi_set_stdio;
typedef int (CALLBACK* GSAPI_INIT_WITH_ARGS)(gs_main_instance *instance, int argc, char **argv);
static GSAPI_INIT_WITH_ARGS ptr_gsapi_init_with_args;

typedef int (CALLBACK* GSAPI_EXIT)(gs_main_instance *instance);
static GSAPI_EXIT ptr_gsapi_exit;

typedef (CALLBACK* GSAPI_RUN_STRING_BEGIN)(gs_main_instance *instance, int user_errors, int *pexit_code);
static GSAPI_RUN_STRING_BEGIN ptr_gsapi_run_string_begin;
typedef (CALLBACK* GSAPI_RUN_STRING_CONTINUE)(gs_main_instance *instance, const char *str, unsigned int length, int user_errors, int *pexit_code);
static GSAPI_RUN_STRING_CONTINUE ptr_gsapi_run_string_continue;
typedef (CALLBACK* GSAPI_RUN_STRING_END)(gs_main_instance *instance, int user_errors, int *pexit_code);
static GSAPI_RUN_STRING_END ptr_gsapi_run_string_end;

static uae_u8 **psbuffer;
static int psbuffers;

static LONG WINAPI ExceptionFilter (struct _EXCEPTION_POINTERS * pExceptionPointers, DWORD ec)
{
	return EXCEPTION_EXECUTE_HANDLER;
}

static void freepsbuffers (void)
{
	int i;
	for (i = 0; i < psbuffers; i++)
		free (psbuffer[i]);
	free (psbuffer);
	psbuffer = NULL;
	psbuffers = 0;
}

static int openprinter_ps (void)
{
	TCHAR *gsargv[] = {
		L"-dNOPAUSE", L"-dBATCH", L"-dNOPAGEPROMPT", L"-dNOPROMPT", L"-dQUIET", L"-dNoCancel",
		L"-sDEVICE=mswinpr2", NULL
	};
	int gsargc, gsargc2, i;
	TCHAR *tmpparms[100];
	TCHAR tmp[MAX_DPATH];
	char *gsparms[100];

	if (ptr_gsapi_new_instance (&gsinstance, NULL) < 0)
		return 0;
	cmdlineparser (currprefs.ghostscript_parameters, tmpparms, 100 - 10);

	gsargc2 = 0;
	gsparms[gsargc2++] = ua (L"WinUAE");
	for (gsargc = 0; gsargv[gsargc]; gsargc++) {
		gsparms[gsargc2++] = ua (gsargv[gsargc]);
	}
	for (i = 0; tmpparms[i]; i++)
		gsparms[gsargc2++] = ua (tmpparms[i]);
	if (currprefs.prtname[0]) {
		_stprintf (tmp, L"-sOutputFile=%%printer%%%s", currprefs.prtname);
		gsparms[gsargc2++] = ua (tmp);
	}
	if (postscript_print_debugging) {
		for (i = 0; i < gsargc2; i++) {
			TCHAR *parm = au (gsparms[i]);
			write_log (L"GSPARM%d: '%s'\n", i, parm);
			xfree (parm);
		}
	}
	__try {
		int rc = ptr_gsapi_init_with_args (gsinstance, gsargc2, gsparms);
		for (i = 0; i < gsargc2; i++) {
			xfree (gsparms[i]);
		}
		if (rc != 0) {
			write_log (L"GS failed, returncode %d\n", rc);
			return 0;
		}
		ptr_gsapi_run_string_begin (gsinstance, 0, &gs_exitcode);
	} __except (ExceptionFilter (GetExceptionInformation (), GetExceptionCode ())) {
		write_log (L"GS crashed\n");
		return 0;
	}
	psmode = 1;
	return 1;
}

static void *prt_thread (void *p)
{
	uae_u8 **buffers = p;
	int err, cnt, ok;

	ok = 1;
	prt_running++;
	prt_started = 1;
	SetThreadPriority (GetCurrentThread (), THREAD_PRIORITY_BELOW_NORMAL);
	if (load_ghostscript ()) {
		if (openprinter_ps ()) {
			write_log (L"PostScript printing emulation started..\n");
			cnt = 0;
			while (buffers[cnt]) {
				uae_u8 *p = buffers[cnt];
				err = ptr_gsapi_run_string_continue (gsinstance, p + 2, (p[0] << 8) | p[1], 0, &gs_exitcode);
				if (err != e_NeedInput && err <= e_Fatal) {
					ptr_gsapi_exit (gsinstance);
					write_log (L"PostScript parsing failed.\n");
					ok = 0;
					break;
				}
				cnt++;
			}
			cnt = 0;
			while (buffers[cnt]) {
				xfree (buffers[cnt]);
				cnt++;
			}
			xfree (buffers);
			if (ok) {
				write_log (L"PostScript printing emulation finished..\n");
				ptr_gsapi_run_string_end (gsinstance, 0, &gs_exitcode);
			}
		} else {
			write_log (L"gsdll32.dll failed to initialize\n");
		}
	} else {
		write_log (L"gsdll32.dll failed to load\n");
	}
	unload_ghostscript ();
	prt_running--;
	return 0;
}

static int doflushprinter (void)
{
	if (prtopen == 0 && prtbufbytes < MIN_PRTBYTES) {
		if (prtbufbytes > 0)
			write_log (L"PRINTER: %d bytes received, less than %d bytes, not printing.\n", prtbufbytes, MIN_PRTBYTES);
		prtbufbytes = 0;
		return 0;
	}
	return 1;
}

static void openprinter (void);
static void flushprtbuf (void)
{
	DWORD written = 0;

	if (!prtbufbytes)
		return;

	if (postscript_print_debugging && prtdump)
		zfile_fwrite (prtbuf, prtbufbytes, 1, prtdump);

	if (currprefs.parallel_postscript_emulation) {

		if (psmode) {
			uae_u8 *p;
			psbuffer = realloc (psbuffer, (psbuffers + 2) * sizeof (uae_u8*));
			p = malloc (prtbufbytes + 2);
			p[0] = prtbufbytes >> 8;
			p[1] = prtbufbytes;
			memcpy (p + 2, prtbuf, prtbufbytes);
			psbuffer[psbuffers++] = p;
			psbuffer[psbuffers] = NULL;
		}
		prtbufbytes = 0;
		return;

	} else if (prtbufbytes > 0) {
		int pbyt = prtbufbytes;

		if (currprefs.parallel_matrix_emulation >= PARALLEL_MATRIX_EPSON) {
			int i;
			if (!prtopen) {
				if (epson_init (currprefs.parallel_matrix_emulation))
					prtopen = 1;
			}
			for (i = 0; i < prtbufbytes; i++)
				epson_printchar (prtbuf[i]);
		} else {
			if (hPrt == INVALID_HANDLE_VALUE) {
				if (!doflushprinter ())
					return;
				openprinter ();
			}
			if (hPrt != INVALID_HANDLE_VALUE) {
				if (WritePrinter (hPrt, prtbuf, pbyt, &written)) {
					if (written != pbyt)
						write_log (L"PRINTER: Only wrote %d of %d bytes!\n", written, pbyt);
				} else {
					write_log (L"PRINTER: Couldn't write data!\n");
				}
			}
		}

	}
	prtbufbytes = 0;
}

void finishjob (void)
{
	flushprtbuf ();
}

static void DoSomeWeirdPrintingStuff (uae_char val)
{
	static uae_char prev[5];

	memmove (prev, prev + 1, 3);
	prev[3] = val;
	prev[4] = 0;
	if (currprefs.parallel_postscript_detection) {
		if (psmode && val == 4) {
			flushprtbuf ();
			*prtbuf = val;
			prtbufbytes = 1;
			flushprtbuf ();
			write_log (L"PostScript end detected..\n");

			if (postscript_print_debugging) {
				zfile_fclose (prtdump);
				prtdump = NULL;
			}

			if (currprefs.parallel_postscript_emulation) {
				prt_started = 0;
				if (uae_start_thread (L"postscript", prt_thread, psbuffer, NULL)) {
					while (!prt_started)
						Sleep (5);
					psbuffers = 0;
					psbuffer = NULL;
				}
			} else {
				closeprinter ();
			}
			freepsbuffers ();
			return;

		} else if (!psmode && !stricmp (prev, "%!PS")) {

			if (postscript_print_debugging)
				prtdump = zfile_fopen (L"psdump.dat", L"wb", 0);

			psmode = 1;
			psbuffer = malloc (sizeof (uae_u8*));
			psbuffer[0] = 0;
			psbuffers = 0;
			strcpy (prtbuf, "%!PS");
			prtbufbytes = strlen (prtbuf);
			flushprtbuf ();
			write_log (L"PostScript start detected..\n");
			return;
		}
	}
	if (prtbufbytes < PRTBUFSIZE) {
		prtbuf[prtbufbytes++] = val;
	} else {
		flushprtbuf ();
		*prtbuf = val;
		prtbufbytes = 1;
	}
}

int isprinter (void)
{
	if (!currprefs.prtname[0])
		return 0;
	if (!_tcsncmp (currprefs.prtname, L"LPT", 3)) {
		paraport_open (currprefs.prtname);
		return -1;
	}
	return 1;
}

int isprinteropen (void)
{
	if (prtopen || prtbufbytes > 0)
		return 1;
	return 0;
}

int load_ghostscript (void)
{
	struct gsapi_revision_s r;
	TCHAR path[MAX_DPATH];
	TCHAR *s;

	if (gsdll)
		return 1;
	_tcscpy (path, L"gsdll32.dll");
	gsdll = WIN32_LoadLibrary (path);
	if (!gsdll) {
		if (GetEnvironmentVariable (L"GS_DLL", path, sizeof (path) / sizeof (TCHAR)))
			gsdll = LoadLibrary (path);
	}
	if (!gsdll) {
		HKEY key;
		DWORD ret = RegOpenKeyEx (HKEY_LOCAL_MACHINE, L"SOFTWARE\\AFPL Ghostscript", 0, KEY_READ, &key);
		if (ret != ERROR_SUCCESS)
			ret = RegOpenKeyEx (HKEY_LOCAL_MACHINE, L"SOFTWARE\\GPL Ghostscript", 0, KEY_READ, &key);
		if (ret == ERROR_SUCCESS) {
			int idx = 0, cnt = 20;
			TCHAR tmp1[MAX_DPATH];
			while (cnt-- > 0) {
				DWORD size1 = sizeof (tmp1) / sizeof (TCHAR);
				FILETIME ft;
				if (RegEnumKeyEx (key, idx, tmp1, &size1, NULL, NULL, NULL, &ft) == ERROR_SUCCESS) {
					HKEY key2;
					if (RegOpenKeyEx (key, tmp1, 0, KEY_READ, &key2) == ERROR_SUCCESS) {
						DWORD type = REG_SZ;
						DWORD size = sizeof (path) / sizeof (TCHAR);
						if (RegQueryValueEx (key2, L"GS_DLL", 0, &type, (LPBYTE)path, &size) == ERROR_SUCCESS) {
							gsdll = LoadLibrary (path);
						}
						RegCloseKey (key2);
						if (gsdll)
							break;
					}
				}
				idx++;
			}
			RegCloseKey (key);
		}
	}
	if (!gsdll)
		return 0;
	ptr_gsapi_revision = (GSAPI_REVISION)GetProcAddress (gsdll, "gsapi_revision");
	if (!ptr_gsapi_revision) {
		unload_ghostscript ();
		write_log (L"incompatible %s! (1)\n", path);
		return -1;
	}
	if (ptr_gsapi_revision(&r, sizeof(r))) {
		unload_ghostscript ();
		write_log (L"incompatible %s! (2)\n", path);
		return -2;
	}
	ptr_gsapi_new_instance = (GSAPI_NEW_INSTANCE)GetProcAddress (gsdll, "gsapi_new_instance");
	ptr_gsapi_delete_instance = (GSAPI_DELETE_INSTANCE)GetProcAddress (gsdll, "gsapi_delete_instance");
	ptr_gsapi_set_stdio = (GSAPI_SET_STDIO)GetProcAddress (gsdll, "gsapi_set_stdio");
	ptr_gsapi_exit = (GSAPI_EXIT)GetProcAddress (gsdll, "gsapi_exit");
	ptr_gsapi_run_string_begin = (GSAPI_RUN_STRING_BEGIN)GetProcAddress (gsdll, "gsapi_run_string_begin");
	ptr_gsapi_run_string_continue = (GSAPI_RUN_STRING_CONTINUE)GetProcAddress (gsdll, "gsapi_run_string_continue");
	ptr_gsapi_run_string_end = (GSAPI_RUN_STRING_END)GetProcAddress (gsdll, "gsapi_run_string_end");
	ptr_gsapi_init_with_args = (GSAPI_INIT_WITH_ARGS)GetProcAddress (gsdll, "gsapi_init_with_args");
	if (!ptr_gsapi_new_instance || !ptr_gsapi_delete_instance || !ptr_gsapi_exit ||
		!ptr_gsapi_run_string_begin || !ptr_gsapi_run_string_continue || !ptr_gsapi_run_string_end ||
		!ptr_gsapi_init_with_args) {
			unload_ghostscript ();
			write_log (L"incompatible %s! (3)\n", path);
			return -3;
	}
	s = au (r.product);
	write_log (L"%s: %s rev %d initialized\n", path, s, r.revision);
	xfree (s);
	return 1;
}

void unload_ghostscript (void)
{
	if (gsinstance) {
		ptr_gsapi_exit (gsinstance);
		ptr_gsapi_delete_instance (gsinstance);
	}
	gsinstance = NULL;
	if (gsdll)
		FreeLibrary (gsdll);
	gsdll = NULL;
	psmode = 0;
}

static void openprinter (void)
{
	DOC_INFO_1 DocInfo;
	static int first;

	closeprinter ();
	if (!currprefs.prtname[0])
		return;

	if (currprefs.parallel_postscript_emulation) {
		prtopen = 1;
		return;
	} else if (currprefs.parallel_matrix_emulation >= PARALLEL_MATRIX_EPSON) {
		epson_init (currprefs.parallel_matrix_emulation);
	} else if (hPrt == INVALID_HANDLE_VALUE) {
		flushprtbuf ();
		if (OpenPrinter (currprefs.prtname, &hPrt, NULL)) {
			// Fill in the structure with info about this "document."
			DocInfo.pDocName = L"WinUAE Document";
			DocInfo.pOutputFile = NULL;
			DocInfo.pDatatype = (currprefs.parallel_matrix_emulation || currprefs.parallel_postscript_detection) ? L"TEXT" : L"RAW";
			// Inform the spooler the document is beginning.
			if ((dwJob = StartDocPrinter (hPrt, 1, (LPSTR)&DocInfo)) == 0) {
				ClosePrinter (hPrt );
				hPrt = INVALID_HANDLE_VALUE;
			} else if (StartPagePrinter (hPrt)) {
				prtopen = 1;
			}
		} else {
			hPrt = INVALID_HANDLE_VALUE; // Stupid bug in Win32, where OpenPrinter fails, but hPrt ends up being zero
		}
	}
	if (hPrt != INVALID_HANDLE_VALUE) {
		write_log (L"PRINTER: Opening printer \"%s\" with handle 0x%x.\n", currprefs.prtname, hPrt);
	} else if (*currprefs.prtname) {
		write_log (L"PRINTER: ERROR - Couldn't open printer \"%s\" for output.\n", currprefs.prtname);
	}
}

void flushprinter (void)
{
	if (!doflushprinter ())
		return;
	flushprtbuf ();
	closeprinter ();
}

void closeprinter (void)
{
#ifdef PRINT_DUMP
	zfile_fclose (prtdump);
#endif
	parflush = 0;
	psmode = 0;
	if (hPrt != INVALID_HANDLE_VALUE) {
		EndPagePrinter (hPrt);
		EndDocPrinter (hPrt);
		ClosePrinter (hPrt);
		hPrt = INVALID_HANDLE_VALUE;
		write_log (L"PRINTER: Closing printer.\n");
	}
	if (currprefs.parallel_postscript_emulation)
		prtopen = 1;
	else
		prtopen = 0;
	if (prt_running) {
		write_log (L"waiting for printing to finish...\n");
		while (prt_running)
			Sleep (10);
	}
	freepsbuffers ();
	epson_close ();
	prtbufbytes = 0;
}

void doprinter (uae_u8 val)
{
	parflush = 0;
	DoSomeWeirdPrintingStuff (val);
}

struct uaeserialdatawin32
{
	HANDLE hCom;
	HANDLE evtr, evtw, evtt, evtwce;
	OVERLAPPED olr, olw, olwce;
	int writeactive;
	void *readdata, *writedata;
	volatile int threadactive;
	uae_sem_t change_sem, sync_sem;
	void *user;
};

int uaeser_getdatalenght (void)
{
	return sizeof (struct uaeserialdatawin32);
}

static void uaeser_initdata (struct uaeserialdatawin32 *sd, void *user)
{
	memset (sd, 0, sizeof (struct uaeserialdatawin32));
	sd->hCom = INVALID_HANDLE_VALUE;
	sd->evtr = sd->evtw = sd->evtt = sd->evtwce = 0;
	sd->user = user;
}

int uaeser_query (struct uaeserialdatawin32 *sd, uae_u16 *status, uae_u32 *pending)
{
	DWORD err, modem;
	COMSTAT ComStat;
	uae_u16 s = 0;

	if (!ClearCommError (sd->hCom, &err, &ComStat))
		return 0;
	*pending = ComStat.cbInQue;
	if (status) {
		s |= (err & CE_BREAK) ? (1 << 10) : 0;
		s |= (err & CE_RXOVER) ? (1 << 8) : 0;
		if (GetCommModemStatus (sd->hCom, &modem)) {
			s |= (modem & MS_CTS_ON) ? 0 : (1 << 4);
			s |= (modem & MS_DSR_ON) ? 0 : (1 << 7);
			s |= (modem & MS_RING_ON) ? (1 << 2) : 0;
		}
		*status = s;
	}
	return 1;
}

int uaeser_break (struct uaeserialdatawin32 *sd, int brklen)
{
	if (!SetCommBreak (sd->hCom))
		return 0;
	Sleep (brklen / 1000);
	ClearCommBreak (sd->hCom);
	return 1;
}

int uaeser_setparams (struct uaeserialdatawin32 *sd, int baud, int rbuffer, int bits, int sbits, int rtscts, int parity, uae_u32 xonxoff)
{
	DCB dcb;

	memset (&dcb, 0, sizeof (dcb));
	dcb.DCBlength = sizeof (DCB);
	if (!GetCommState (sd->hCom, &dcb))
		return 5;

	dcb.fBinary = TRUE;
	dcb.BaudRate = baud;
	dcb.ByteSize = bits;
	dcb.Parity = parity == 0 ? NOPARITY : (parity == 1 ? ODDPARITY : EVENPARITY);
	dcb.fParity = FALSE;
	dcb.StopBits = sbits == 1 ? ONESTOPBIT : TWOSTOPBITS;

	dcb.fDsrSensitivity = FALSE;
	dcb.fOutxDsrFlow = FALSE;
	dcb.fDtrControl = DTR_CONTROL_DISABLE;

	if (rtscts) {
		dcb.fOutxCtsFlow = TRUE;
		dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
	} else {
		dcb.fRtsControl = RTS_CONTROL_DISABLE;
		dcb.fOutxCtsFlow = FALSE;
	}

	dcb.fTXContinueOnXoff = FALSE;
	if (xonxoff & 1) {
		dcb.fOutX = TRUE;
		dcb.fInX = TRUE;
		dcb.XonChar = (xonxoff >> 8) & 0xff;
		dcb.XoffChar = (xonxoff >> 16) & 0xff;
	} else {
		dcb.fOutX = FALSE;
		dcb.fInX = FALSE;
	}

	dcb.fErrorChar = FALSE;
	dcb.fNull = FALSE;
	dcb.fAbortOnError = FALSE;

	//dcb.XoffLim = 512;
	//dcb.XonLim = 2048;

	if (!SetCommState (sd->hCom, &dcb)) {
		write_log (L"uaeserial: SetCommState() failed %d\n", GetLastError());
		return 5;
	}
	SetupComm (sd->hCom, rbuffer, rbuffer);
	return 0;
}

static void startwce(struct uaeserialdatawin32 *sd, DWORD *evtmask)
{
	SetEvent(sd->evtwce);
	WaitCommEvent(sd->hCom, evtmask, &sd->olwce);
}

static void *uaeser_trap_thread (void *arg)
{
	struct uaeserialdatawin32 *sd = arg;
	HANDLE handles[4];
	int cnt, actual;
	DWORD evtmask;

	uae_set_thread_priority (NULL, 1);
	sd->threadactive = 1;
	uae_sem_post (&sd->sync_sem);
	startwce(sd, &evtmask);
	while (sd->threadactive == 1) {
		int sigmask = 0;
		uae_sem_wait (&sd->change_sem);
		if (WaitForSingleObject(sd->evtwce, 0) == WAIT_OBJECT_0) {
			if (evtmask & EV_RXCHAR)
				sigmask |= 1;
			if ((evtmask & EV_TXEMPTY) && !sd->writeactive)
				sigmask |= 2;
			startwce(sd, &evtmask);
		}
		cnt = 0;
		handles[cnt++] = sd->evtt;
		handles[cnt++] = sd->evtwce;
		if (sd->writeactive) {
			if (GetOverlappedResult (sd->hCom, &sd->olw, &actual, FALSE)) {
				sd->writeactive = 0;
				sigmask |= 2;
			} else {
				handles[cnt++] = sd->evtw;
			}
		}
		if (!sd->writeactive)
			sigmask |= 2;
		uaeser_signal (sd->user, sigmask | 1);
		uae_sem_post (&sd->change_sem);
		WaitForMultipleObjects(cnt, handles, FALSE, INFINITE);
	}
	sd->threadactive = 0;
	uae_sem_post (&sd->sync_sem);
	return 0;
}

void uaeser_trigger (struct uaeserialdatawin32 *sd)
{
	SetEvent (sd->evtt);
}

int uaeser_write (struct uaeserialdatawin32 *sd, uae_u8 *data, uae_u32 len)
{
	int ret = 1;
	if (!WriteFile (sd->hCom, data, len, NULL, &sd->olw)) {
		sd->writeactive = 1;
		if (GetLastError() != ERROR_IO_PENDING) {
			ret = 0;
			sd->writeactive = 0;
		}
	}
	SetEvent (sd->evtt);
	return ret;
}

int uaeser_read (struct uaeserialdatawin32 *sd, uae_u8 *data, uae_u32 len)
{
	int ret = 1;
	DWORD err;
	COMSTAT ComStat;

	if (!ClearCommError (sd->hCom, &err, &ComStat))
		return 0;
	if (len > ComStat.cbInQue)
		return 0;
	if (!ReadFile (sd->hCom, data, len, NULL, &sd->olr)) {
		if (GetLastError() == ERROR_IO_PENDING)
			WaitForSingleObject(sd->evtr, INFINITE);
		else
			ret = 0;
	}
	SetEvent (sd->evtt);
	return ret;
}

void uaeser_clearbuffers (struct uaeserialdatawin32 *sd)
{
	PurgeComm (sd->hCom, PURGE_TXCLEAR | PURGE_RXCLEAR);
}

int uaeser_open (struct uaeserialdatawin32 *sd, void *user, int unit)
{
	TCHAR buf[256];
	COMMTIMEOUTS CommTimeOuts;

	sd->user = user;
	_stprintf (buf, L"\\\\.\\COM%d", unit);
	sd->evtr = CreateEvent (NULL, TRUE, FALSE, NULL);
	sd->evtw = CreateEvent (NULL, TRUE, FALSE, NULL);
	sd->evtt = CreateEvent (NULL, FALSE, FALSE, NULL);
	sd->evtwce = CreateEvent (NULL, TRUE, FALSE, NULL);
	if (!sd->evtt || !sd->evtw || !sd->evtt || !sd->evtwce)
		goto end;
	sd->olr.hEvent = sd->evtr;
	sd->olw.hEvent = sd->evtw;
	sd->olwce.hEvent = sd->evtwce;
	sd->hCom = CreateFile (buf, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
	if (sd->hCom == INVALID_HANDLE_VALUE) {
		_stprintf (buf, L"\\.\\\\COM%d", unit);
		sd->hCom = CreateFile (buf, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
		if (sd->hCom == INVALID_HANDLE_VALUE) {
			write_log (L"UAESER: '%s' failed to open, err=%d\n", buf, GetLastError());
			goto end;
		}
	}
	uae_sem_init (&sd->sync_sem, 0, 0);
	uae_sem_init (&sd->change_sem, 0, 1);
	uae_start_thread (L"uaeserial_win32", uaeser_trap_thread, sd, NULL);
	uae_sem_wait (&sd->sync_sem);

	CommTimeOuts.ReadIntervalTimeout = 0;
	CommTimeOuts.ReadTotalTimeoutMultiplier = 0;
	CommTimeOuts.ReadTotalTimeoutConstant = 0;
	CommTimeOuts.WriteTotalTimeoutMultiplier = 0;
	CommTimeOuts.WriteTotalTimeoutConstant = 0;
	SetCommTimeouts (sd->hCom, &CommTimeOuts);
	SetCommMask (sd->hCom, EV_RXCHAR | EV_TXEMPTY | EV_BREAK);

	return 1;

end:
	uaeser_close (sd);
	return 0;
}

void uaeser_close (struct uaeserialdatawin32 *sd)
{
	if (sd->threadactive) {
		sd->threadactive = -1;
		SetEvent (sd->evtt);
		while (sd->threadactive)
			Sleep(10);
		CloseHandle (sd->evtt);
	}
	if (sd->hCom != INVALID_HANDLE_VALUE)
		CloseHandle(sd->hCom);
	if (sd->evtr)
		CloseHandle(sd->evtr);
	if (sd->evtw)
		CloseHandle(sd->evtw);
	if (sd->evtwce)
		CloseHandle(sd->evtwce);
	uaeser_initdata (sd, sd->user);
}

static HANDLE hCom = INVALID_HANDLE_VALUE;
static DCB dcb;
static HANDLE writeevent, readevent;
#define SERIAL_WRITE_BUFFER 100
#define SERIAL_READ_BUFFER 100
static uae_u8 outputbuffer[SERIAL_WRITE_BUFFER];
static uae_u8 outputbufferout[SERIAL_WRITE_BUFFER];
static uae_u8 inputbuffer[SERIAL_READ_BUFFER];
static int datainoutput;
static int dataininput, dataininputcnt;
static OVERLAPPED writeol, readol;
static writepending;

int openser (TCHAR *sername)
{
	COMMTIMEOUTS CommTimeOuts;

	if (!(readevent = CreateEvent (NULL, TRUE, FALSE, NULL))) {
		write_log (L"SERIAL: Failed to create r event!\n");
		return 0;
	}
	readol.hEvent = readevent;

	if (!(writeevent = CreateEvent (NULL, TRUE, FALSE, NULL))) {
		write_log (L"SERIAL: Failed to create w event!\n");
		return 0;
	}
	SetEvent (writeevent);
	writeol.hEvent = writeevent;

	uartbreak = 0;

	hCom = CreateFile (sername, GENERIC_READ | GENERIC_WRITE,
		0,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
		NULL);
	if (hCom == INVALID_HANDLE_VALUE) {
		write_log (L"SERIAL: failed to open '%s' err=%d\n", sername, GetLastError());
		closeser ();
		return 0;
	}

	SetCommMask (hCom, EV_RXFLAG);
	SetupComm (hCom, 65536, 128);
	PurgeComm (hCom, PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR);
	CommTimeOuts.ReadIntervalTimeout = 0xFFFFFFFF;
	CommTimeOuts.ReadTotalTimeoutMultiplier = 0;
	CommTimeOuts.ReadTotalTimeoutConstant = 0;
	CommTimeOuts.WriteTotalTimeoutMultiplier = 0;
	CommTimeOuts.WriteTotalTimeoutConstant = 0;
	SetCommTimeouts (hCom, &CommTimeOuts);

	dcb.DCBlength = sizeof (DCB);
	GetCommState (hCom, &dcb);

	dcb.fBinary = TRUE;
	dcb.BaudRate = 9600;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.fParity = FALSE;
	dcb.StopBits = ONESTOPBIT;

	dcb.fDsrSensitivity = FALSE;
	dcb.fOutxDsrFlow = FALSE;
	dcb.fDtrControl = DTR_CONTROL_DISABLE;

	if (currprefs.serial_hwctsrts) {
		dcb.fOutxCtsFlow = TRUE;
		dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
	} else {
		dcb.fRtsControl = RTS_CONTROL_DISABLE;
		dcb.fOutxCtsFlow = FALSE;
	}

	dcb.fTXContinueOnXoff = FALSE;
	dcb.fOutX = FALSE;
	dcb.fInX = FALSE;

	dcb.fErrorChar = FALSE;
	dcb.fNull = FALSE;
	dcb.fAbortOnError = FALSE;

	//dcb.XoffLim = 512;
	//dcb.XonLim = 2048;

	if (SetCommState (hCom, &dcb)) {
		write_log (L"SERIAL: Using %s CTS/RTS=%d\n", sername, currprefs.serial_hwctsrts);
		return 1;
	}

	write_log (L"SERIAL: serial driver didn't accept new parameters\n");
	closeser();
	return 0;
}

void closeser (void)
{
	if (hCom != INVALID_HANDLE_VALUE)  {
		CloseHandle (hCom);
		hCom = INVALID_HANDLE_VALUE;
	}
	if (midi_ready) {
		extern uae_u16 serper;
		Midi_Close ();
		//need for camd Midi Stuff(it close midi and reopen it but serial.c think the baudrate
		//is the same and do not open midi), so setting serper to different value helps
		serper = 0x30;
	}
	if(writeevent)
		CloseHandle(writeevent);
	writeevent = 0;
	if(readevent)
		CloseHandle(readevent);
	readevent = 0;
	uartbreak = 0;
}

static void outser (void)
{
	DWORD actual;
	if (WaitForSingleObject (writeevent, 0) == WAIT_OBJECT_0 && datainoutput > 0) {
		memcpy (outputbufferout, outputbuffer, datainoutput);
		WriteFile (hCom, outputbufferout, datainoutput, &actual, &writeol);
		datainoutput = 0;
	}
}

void writeser (int c)
{
	if (midi_ready) {
		BYTE outchar = (BYTE)c;
		Midi_Parse (midi_output, &outchar);
	} else {
		if (!currprefs.use_serial)
			return;
		if (datainoutput + 1 < sizeof (outputbuffer)) {
			outputbuffer[datainoutput++] = c;
		} else {
			write_log (L"serial output buffer overflow, data will be lost\n");
			datainoutput = 0;
		}
		outser ();
	}
}

int checkserwrite (void)
{
	if (hCom == INVALID_HANDLE_VALUE || !currprefs.use_serial)
		return 1;
	if (midi_ready) {
		return 1;
	} else {
		outser ();
		if (datainoutput >= sizeof (outputbuffer) - 1)
			return 0;
	}
	return 1;
}

int readseravail (void)
{
	COMSTAT ComStat;
	DWORD dwErrorFlags;
	if (midi_ready) {
		if (ismidibyte ())
			return 1;
	} else {
		if (!currprefs.use_serial)
			return 0;
		if (dataininput > dataininputcnt)
			return 1;
		if (hCom != INVALID_HANDLE_VALUE)  {
			ClearCommError (hCom, &dwErrorFlags, &ComStat);
			if (ComStat.cbInQue > 0)
				return 1;
		}
	}
	return 0;
}

int readser (int *buffer)
{
	COMSTAT ComStat;
	DWORD dwErrorFlags;
	DWORD actual;


	if (midi_ready) {
		*buffer = getmidibyte ();
		if (*buffer < 0)
			return 0;
		return 1;
	} else {
		if (!currprefs.use_serial)
			return 0;
		if (dataininput > dataininputcnt) {
			*buffer = inputbuffer[dataininputcnt++];
			return 1;
		}
		dataininput = 0;
		dataininputcnt = 0;
		if (hCom != INVALID_HANDLE_VALUE)  {
			/* only try to read number of bytes in queue */
			ClearCommError (hCom, &dwErrorFlags, &ComStat);
			if (ComStat.cbInQue)  {
				int len = ComStat.cbInQue;
				if (len > sizeof (inputbuffer))
					len = sizeof (inputbuffer);
				if (!ReadFile (hCom, inputbuffer, len, &actual, &readol))  {
					if (GetLastError() == ERROR_IO_PENDING)
						WaitForSingleObject (&readol, INFINITE);
					else
						return 0;
				}
				dataininput = actual;
				dataininputcnt = 0;
				if (actual == 0)
					return 0;
				return readser (buffer);
			}
		}
	}
	return 0;
}

void serialuartbreak (int v)
{
	if (hCom == INVALID_HANDLE_VALUE || !currprefs.use_serial)
		return;

	if (v)
		EscapeCommFunction (hCom, SETBREAK);
	else
		EscapeCommFunction (hCom, CLRBREAK);
}

void getserstat (int *pstatus)
{
	DWORD stat;
	int status = 0;

	*pstatus = 0;
	if (hCom == INVALID_HANDLE_VALUE || !currprefs.use_serial)
		return;

	GetCommModemStatus (hCom, &stat);
	if (stat & MS_CTS_ON)
		status |= TIOCM_CTS;
	if (stat & MS_RLSD_ON)
		status |= TIOCM_CAR;
	if (stat & MS_DSR_ON)
		status |= TIOCM_DSR;
	if (stat & MS_RING_ON)
		status |= TIOCM_RI;
	*pstatus = status;
}


void setserstat (int mask, int onoff)
{
	if (!currprefs.use_serial || hCom == INVALID_HANDLE_VALUE)
		return;

	if (mask & TIOCM_DTR)
		EscapeCommFunction (hCom, onoff ? SETDTR : CLRDTR);
	if (!currprefs.serial_hwctsrts) {
		if (mask & TIOCM_RTS)
			EscapeCommFunction (hCom, onoff ? SETRTS : CLRRTS);
	}
}

int setbaud (long baud)
{
	if(baud == 31400 && currprefs.win32_midioutdev >= -1) {
		/* MIDI baud-rate */
		if (!midi_ready) {
			if (Midi_Open())
				write_log (L"Midi enabled\n");
		}
		return 1;
	} else {
		if (midi_ready) {
			Midi_Close();
		}
		if (!currprefs.use_serial)
			return 1;
		if (hCom != INVALID_HANDLE_VALUE)  {
			if (GetCommState (hCom, &dcb))  {
				dcb.BaudRate = baud;
				if (!SetCommState (hCom, &dcb)) {
					write_log (L"SERIAL: Error setting baud rate %d!\n", baud);
					return 0;
				}
			} else {
				write_log (L"SERIAL: setbaud internal error!\n");
			}
		}
	}
	return 1;
}

void initparallel (void)
{
	if (uae_boot_rom) {
		uaecptr a = here (); //this install the ahisound
		org (rtarea_base + 0xFFC0);
		calltrap (deftrapres (ahi_demux, 0, L"ahi_winuae"));
		dw (RTS);
		org (a);
		init_ahi_v2 ();
	}
}

int flashscreen;

void doflashscreen (void)
{
	flashscreen = 10;
	init_colors ();
	picasso_refresh ();
	reset_drawing ();
	flush_screen (0, 0);
}

void hsyncstuff (void)
	//only generate Interrupts when
	//writebuffer is complete flushed
	//check state of lwin rwin
{
	static int keycheck = 0;

#ifdef AHI
	{ //begin ahi_sound
		static int count;
		if (ahi_on) {
			count++;
			//15625/count freebuffer check
			if(count > ahi_pollrate) {
				ahi_updatesound (1);
				count = 0;
			}
		}
	} //end ahi_sound
#endif
#ifdef PARALLEL_PORT
	keycheck++;
	if(keycheck >= 1000)
	{
		if (prtopen)
			flushprtbuf ();
		{
			if (flashscreen > 0) {
				flashscreen--;
				if (flashscreen == 0) {
					init_colors ();
					reset_drawing ();
					picasso_refresh ();
					flush_screen (0, 0);
				}
			}
		}
		keycheck = 0;
	}
	if (currprefs.parallel_autoflush_time && !currprefs.parallel_postscript_detection) {
		parflush++;
		if (parflush / ((currprefs.ntscmode ? MAXVPOS_NTSC : MAXVPOS_PAL) * MAXHPOS_PAL / maxhpos) >= currprefs.parallel_autoflush_time * 50) {
			flushprinter ();
			parflush = 0;
		}
	}
#endif
}

static int enumserialports_2 (int cnt)
{
	// Create a device information set that will be the container for
	// the device interfaces.
	HDEVINFO hDevInfo = INVALID_HANDLE_VALUE;
	SP_DEVICE_INTERFACE_DETAIL_DATA *pDetData = NULL;
	BOOL bOk = TRUE;
	SP_DEVICE_INTERFACE_DATA ifcData;
	DWORD dwDetDataSize = sizeof (SP_DEVICE_INTERFACE_DETAIL_DATA) + 256 * sizeof (TCHAR);
	DWORD ii;

	hDevInfo = SetupDiGetClassDevs (&GUID_CLASS_COMPORT, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if(hDevInfo == INVALID_HANDLE_VALUE)
		return 0;
	// Enumerate the serial ports
	pDetData = xmalloc (dwDetDataSize);
	// This is required, according to the documentation. Yes,
	// it's weird.
	ifcData.cbSize = sizeof (SP_DEVICE_INTERFACE_DATA);
	pDetData->cbSize = sizeof (SP_DEVICE_INTERFACE_DETAIL_DATA);
	for (ii = 0; bOk; ii++) {
		bOk = SetupDiEnumDeviceInterfaces (hDevInfo, NULL, &GUID_CLASS_COMPORT, ii, &ifcData);
		if (bOk) {
			// Got a device. Get the details.
			SP_DEVINFO_DATA devdata = { sizeof (SP_DEVINFO_DATA)};
			bOk = SetupDiGetDeviceInterfaceDetail (hDevInfo,
				&ifcData, pDetData, dwDetDataSize, NULL, &devdata);
			if (bOk) {
				// Got a path to the device. Try to get some more info.
				TCHAR fname[256];
				TCHAR desc[256];
				BOOL bSuccess = SetupDiGetDeviceRegistryProperty (
					hDevInfo, &devdata, SPDRP_FRIENDLYNAME, NULL,
					(PBYTE)fname, sizeof (fname), NULL);
				bSuccess = bSuccess && SetupDiGetDeviceRegistryProperty (
					hDevInfo, &devdata, SPDRP_DEVICEDESC, NULL,
					(PBYTE)desc, sizeof (desc), NULL);
				if (bSuccess && cnt < MAX_SERIAL_PORTS) {
					TCHAR *p;
					comports[cnt].dev = my_strdup (pDetData->DevicePath);
					comports[cnt].name = my_strdup (fname);
					p = _tcsstr (fname, L"(COM");
					if (p && (p[5] == ')' || p[6] == ')')) {
						comports[cnt].cfgname = xmalloc (100 * sizeof (TCHAR));
						if (isdigit(p[5]))
							_stprintf (comports[cnt].cfgname, L"COM%c%c", p[4], p[5]);
						else
							_stprintf (comports[cnt].cfgname, L"COM%c", p[4]);
					} else {
						comports[cnt].cfgname = my_strdup (pDetData->DevicePath);
					}
					write_log (L"SERPORT: '%s' = '%s' = '%s'\n", comports[cnt].name, comports[cnt].cfgname, comports[cnt].dev);
					cnt++;
				}
			} else {
				write_log (L"SetupDiGetDeviceInterfaceDetail failed, err=%d", GetLastError ());
				goto end;
			}
		} else {
			DWORD err = GetLastError ();
			if (err != ERROR_NO_MORE_ITEMS) {
				write_log (L"SetupDiEnumDeviceInterfaces failed, err=%d", err);
				goto end;
			}
		}
	}
end:
	xfree(pDetData);
	if (hDevInfo != INVALID_HANDLE_VALUE)
		SetupDiDestroyDeviceInfoList (hDevInfo);
	return cnt;
}

int enumserialports (void)
{
	int cnt, i, j;
	TCHAR name[256];
	DWORD size = sizeof (COMMCONFIG);
	TCHAR devname[1000];

	write_log (L"Serial port enumeration..\n");
	cnt = 0;

#ifdef SERIAL_ENET
	comports[cnt].dev = my_strdup (L"ENET:H");
	comports[cnt].cfgname = my_strdup (comports[0].dev);
	comports[cnt].name = my_strdup (L"NET (host)");
	cnt++;
	comports[cnt].dev = my_strdup (L"ENET:L");
	comports[cnt].cfgname = my_strdup (comports[1].dev);
	comports[cnt].name = my_strdup (L"NET (client)");
	cnt++;
#endif

	cnt = enumserialports_2 (cnt);
	for (i = 0; i < 10; i++) {
		_stprintf (name, L"COM%d", i);
		if (!QueryDosDevice (name, devname, sizeof devname / sizeof (TCHAR)))
			continue;
		for(j = 0; j < cnt; j++) {
			if (!_tcscmp (comports[j].cfgname, name))
				break;
		}
		if (j == cnt) {
			if (cnt >= MAX_SERIAL_PORTS)
				break;
			comports[j].dev = xmalloc (100 * sizeof (TCHAR));
			_stprintf (comports[cnt].dev, L"\\.\\\\%s", name);
			comports[j].cfgname = my_strdup (name);
			comports[j].name = my_strdup (name);
			write_log (L"SERPORT: %d:'%s' = '%s' (%s)\n", cnt, comports[j].name, comports[j].dev, devname);
			cnt++;
		}
	}
	write_log (L"Serial port enumeration end\n");
	return cnt;
}

void sernametodev (TCHAR *sername)
{
	int i;

	for (i = 0; i < MAX_SERIAL_PORTS && comports[i].name; i++) {
		if (!_tcscmp(sername, comports[i].cfgname)) {
			_tcscpy (sername, comports[i].dev);
			return;
		}
	}
	sername[0] = 0;
}

void serdevtoname (TCHAR *sername)
{
	int i;
	for (i = 0; i < MAX_SERIAL_PORTS && comports[i].name; i++) {
		if (!_tcscmp(sername, comports[i].dev)) {
			_tcscpy (sername, comports[i].cfgname);
			return;
		}
	}
	sername[0] = 0;
}
