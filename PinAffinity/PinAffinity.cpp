// PinAffinity - application entrypoint and main window

#include "stdafx.h"
#include <time.h>
#include "PinAffinity.h"
#include "ProcessList.h"
#include "SavedProcess.h"
#include "FindParentMenu.h"
#include "LogError.h"
#include "Version.h"
#include "BuildInfo/BuildInfo.h"

// Declare Common Controls usage in our manifest
#pragma comment(linker,"\"/manifestdependency:type='win32' \
	name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
	processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Private messages
const UINT PAMSG_UPDATE = WM_USER + 1;			// update the UI list
const UINT PAMSG_TRAY = WM_USER + 2;			// system tray icon notification

// Timer IDs
const WPARAM TIMER_UPDATE = 1;					// process list update timer
const WPARAM TIMER_MINIMIZE = 2;				// window minimized

// main window class
const TCHAR* g_szWindowClass = _T("Pinscape.PinAffinity.MainWindow");

// tray icon ID
const UINT TRAY_ICON_ID = 1;

const UINT TIMER_UPDATE_TIMEOUT = 200;

// Globals
#define MAX_LOADSTRING 100
HINSTANCE g_hInst;                              // current instance
TCHAR g_szTitle[MAX_LOADSTRING];                // main the title bar text
TCHAR g_szErrTitle[MAX_LOADSTRING];             // title bar caption for error dialogs
HWND g_hWnd;									// main window handle
HWND g_hListView;								// main window list view
HMENU g_hMenuListView;							// list view context menu
HMENU g_hMenuTray;								// tray icon menu
TCHAR g_szConfigFile[MAX_PATH];                 // config file name

TCHAR g_szRunning[MAX_LOADSTRING];				// "Running" status text
BOOL g_configDirty = false;						// config has unsaved changes
DWORD_PTR g_sysAffinityMask;                    // system CPU affinity mask for my own process

// list view columns indices
int g_lvNameCol;
int g_lvPidCol;
int g_lvStatusCol;
int g_lvTypeCol;
int g_lvAffCol;
int g_lvStartedCol;


// Process type list
std::vector<ProcTypeDesc> g_procTypes;

// Saved process table
std::unordered_map<TSTRING, SavedProc> g_savedProcs;

// Current active process list, by process ID
std::unordered_map<DWORD, ProcListItem> g_curProcList;


// Forward declarations
ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);
BOOL AdjustPrivileges(void);
void ErrorBox(int messageResID);
void LoadProcessTypes();
void PopulateProcTypeMenu(HMENU hmenu);
void RestoreOriginalAffinities();
void GetAppFilePath(TCHAR* buf, const TCHAR* fname);
void LoadConfig();
bool SaveConfig();

// Main program entrypoint
int APIENTRY wWinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR lpCmdLine,
	_In_ int nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);

	// parse the command line
	for (int i = 1; i < __argc; ++i)
	{
		if (_tcsicmp(__targv[i], _T("/minimize")) == 0)
		{
			//  /Minimize -> start with the window minimized
			nCmdShow = SW_SHOWMINNOACTIVE;
		}
		else
		{
			ErrorBox(IDS_ERR_USAGE);
			return 0;
		}
	}

	// seed the random number generator
	::srand((unsigned int)::time(NULL));

	// Since this application has a system-wide effect, it's not useful
	// to run multiple instances at once.  Look for existing instances.
	// Iterate a few times just in case we encounter a weird timing
	// condition where the other instance is just starting up or just
	// shutting down - but don't loop forever so that we don't get into
	// a deadlock with another program.
	HANDLE hProgramMutex = 0;
	for (int tries = 0; ; ++tries)
	{
		// Create a named mutex.  If there's another instance, it will
		// already have created the same mutex, so we'll get an "already
		// exists" error.
		hProgramMutex = CreateMutex(0, TRUE, _T("Pinscape.PinAffinity.InstanceMutex"));
		DWORD err = GetLastError();
		if (hProgramMutex != NULL && err == 0)
		{
			// Success - we're the proud owner of a shiny new mutex, so we're
			// the only instance of the program running.  We can now proceed
			// to run normally, secure in the knowledge that any other instance
			// launched from this time forward will see that we're running by
			// virtue of the mutex's prior existence.  We'll hold onto the
			// mutex for the rest of our run.
			break;
		}
		else if (err == ERROR_ALREADY_EXISTS)
		{
			// The mutex already exists, which means that another instance of
			// the program is already running.  We got a handle to (but not
			// ownership of) the existing mutex.  We don't need that handle
			// for anything - discard it.
			CloseHandle(hProgramMutex);

			// Look for the existing window
			HWND hWndPrev = FindWindowEx(0, 0, g_szWindowClass, 0);
			if (hWndPrev != 0)
			{
				// Got it.  Send it a system tray Restore command to restore
				// it from hidden or minimized and bring it to the foreground.
				SendMessage(hWndPrev, WM_COMMAND, ID_TRAYMENU_SHOW, 0);

				// the existing instance will take it from here
				return 0;
			}
		}
		else
		{
			// On any other error, abort with an error message about the mutex
			ErrorBox(IDS_PROGMUTEX_ERR);
			return 0;
		}

		// If we've retried too many times already, abort.  There's no point in
		// trying this more than a few times, because the whole point is that
		// there might be a brief interval during normal startup or shutdown
		// where the mutex exists but the window doesn't.  If another instance
		// is just starting up, it will have created the mutex initially, but
		// might not have gotten around to window creation yet; if we wait a
		// moment, it should create its window and we can go about deferring
		// to the existing process.  If another instance is just shutting down,
		// it might have already destroyed its window but hasn't released its
		// mutex yet; if we wait a moment, the mutex will disappear and we'll
		// be able to create it after all.  But both of these cases should
		// resolve almost instantly, since the other process should get on 
		// with that next step as soon as it gets a chance to run - in both
		// cases, the window between the two interlocking steps is short.  If
		// the situation doesn't resolve quickly, there must be something
		// else unexpected going on, probably something pathological, that
		// we can't expect to *ever* resolve itself.  So if we kept looping, 
		// we could get stuck here forever.  Best to give it a couple of tries
		// and give up.
		if (tries > 3)
		{
			// Show an error and exit.  The error is important because something
			// is probably wedged that requires user attention: e.g., maybe the 
			// other process is stuck in a loop, or deadlocked.  If we exited
			// silently, the user would be left wondering why launching the
			// program keeps having no effect.
			ErrorBox(IDS_PROGINSTANCE_ERR);
			return 0;
		}

		// wait for a brief random interval to give the other instance a
		// chance to finish starting up or shutting down
		DWORD dt = (DWORD)(100.0f + (float(::rand()) / RAND_MAX * 1500.0f));
		Sleep(dt);
	}

	// Initialize global strings
	LoadString(hInstance, IDS_APP_TITLE, g_szTitle, MAX_LOADSTRING);
	LoadString(hInstance, IDS_ERR_TITLE, g_szErrTitle, MAX_LOADSTRING);
	LoadString(hInstance, IDS_RUNNING, g_szRunning, MAX_LOADSTRING);
	MyRegisterClass(hInstance);

	// adjust privileges to allow access to process information across the system
	if (!AdjustPrivileges())
	{
		ErrorBox(IDS_SETPRIV_ERR);
		return 0;
	}

	// initialize common controls
	InitCommonControls();

	// load the process types
	LoadProcessTypes();

	// read the config file
	GetAppFilePath(g_szConfigFile, _T("SavedProcesses.txt"));
	LoadConfig();

	// get my own process's affinity mask
	g_sysAffinityMask = ~(DWORD_PTR)0;
	HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
	if (hProc != 0)
	{
		DWORD_PTR aff;
		GetProcessAffinityMask(hProc, &aff, &g_sysAffinityMask);
		CloseHandle(hProc);
	}

	// create our window
	if (!InitInstance(hInstance, nCmdShow))
		return FALSE;

	// load the list view context menu
	g_hMenuListView = LoadMenu(g_hInst, MAKEINTRESOURCE(IDR_LISTMENU));
	PopulateProcTypeMenu(g_hMenuListView);

	// load the tray menu
	g_hMenuTray = LoadMenu(g_hInst, MAKEINTRESOURCE(IDR_TRAYMENU));

	// load accelerators
	HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_PINAFFINITY));

	// Main message loop
	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0))
	{
		if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	// Restore original affinities
	RestoreOriginalAffinities();

	// try saving the configuration if it's dirty
	if (g_configDirty)
	{
		// try saving until we either succeed or the user cancels out of the error dialog
		while (!SaveConfig())
		{
			// show a Retry/Cancel dialog explaining the error
			TCHAR buf[1024];
			_stprintf_s(buf, _T("An error occurred saving PinAffinity configuration updates to ")
				_T("file \"%s\".  If you want to try again, check that the folder exists and ")
				_T("that no other program is using the file."),
				g_szConfigFile);
			int btn = MessageBox(0, buf, g_szErrTitle, MB_RETRYCANCEL | MB_ICONERROR);

			// if they canceled, don't try again
			if (btn == IDCANCEL)
				break;
		}
	}

	// release and destroy our program instance mutex
	ReleaseMutex(hProgramMutex);
	CloseHandle(hProgramMutex);

	// return the WM_QUIT code
	return (int)msg.wParam;
}

// show a message box with the given flags, showing the given string resource
// as the message
DWORD MsgBox(int messageResID, DWORD flags)
{
	// load the message
	TCHAR buf[4096];
	LoadString(g_hInst, messageResID, buf, countof(buf));

	// show the dialog and return the result
	return MessageBox(0, buf, g_szErrTitle, flags);
}

// show a message box with a simple OK button and the given string resource
// as the message
void ErrorBox(int messageResID)
{
	MsgBox(messageResID, MB_OK | MB_ICONERROR);
}

// adjust thread privileges to enable process information access
BOOL AdjustPrivileges(void)
{
	// We need SeDebugPrivilege to access information on other processes.
	// Get our security token.
	HandleHolder hToken;
	BOOL ok = OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, TRUE, &hToken);
	if (!ok)
	{
		if (GetLastError() == ERROR_NO_TOKEN && ImpersonateSelf(SecurityImpersonation))
			ok = OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, TRUE, &hToken);
	}

	if (!ok)
		return FALSE;

	// get the local ID for SeDebugPrivilege
	LUID luid;
	if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luid))
		return FALSE;

	// enable the privilege
	TOKEN_PRIVILEGES tp;
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL))
		return FALSE;

	// success
	return TRUE;
}

// register my window class
ATOM MyRegisterClass(HINSTANCE hInstance)
{
	WNDCLASSEXW wcex;

	wcex.cbSize = sizeof(WNDCLASSEX);

	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = hInstance;
	wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_PINAFFINITY));
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_PINAFFINITY);
	wcex.lpszClassName = g_szWindowClass;
	wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_PINAFFINITY));

	return RegisterClassExW(&wcex);
}

// Initialize
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
	g_hInst = hInstance;

	g_hWnd = CreateWindow(g_szWindowClass, g_szTitle, WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

	if (g_hWnd == NULL)
		return FALSE;

	ShowWindow(g_hWnd, nCmdShow);
	UpdateWindow(g_hWnd);

	// if we started minimized, hide the window so that we only show the
	// tray icon
	switch (nCmdShow)
	{
	case SW_MINIMIZE:
	case SW_SHOWMINIMIZED:
	case SW_SHOWMINNOACTIVE:
		// hide the window
		ShowWindow(g_hWnd, SW_HIDE);
		break;
	}

	return TRUE;
}

// create the system tray icon
static void CreateSysTrayIcon(HWND hwnd)
{
	// load the tray icon
	HICON hIcon;
	LoadIconMetric(g_hInst, MAKEINTRESOURCE(IDI_PINAFFINITY), LIM_SMALL, &hIcon);

	// set up our tray notification
	NOTIFYICONDATA nid;
	ZeroMemory(&nid, sizeof(nid));
	nid.cbSize = sizeof(nid);
	nid.hWnd = hwnd;
	nid.uID = TRAY_ICON_ID;
	nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	nid.uCallbackMessage = PAMSG_TRAY;
	nid.hIcon = hIcon;
	_tcscpy_s(nid.szTip, _T("PinAffinity - CPU Affinity settings"));
	nid.dwState = 0;
	nid.dwStateMask = 0;
	nid.szInfo[0] = 0;
	nid.szInfoTitle[0] = 0;
	nid.dwInfoFlags = 0;

	// add the icon
	Shell_NotifyIcon(NIM_ADD, &nid);
}

// remove the system tray icon
void RemoveSysTrayIcon(HWND hWnd)
{
	// set up the identifying data
	NOTIFYICONDATA nid;
	nid.cbSize = sizeof(nid);
	nid.hWnd = hWnd;
	nid.uID = TRAY_ICON_ID;
	nid.uFlags = 0;

	// remove it
	Shell_NotifyIcon(NIM_DELETE, &nid);
}

// create the list view
void CreateListView(HWND hWnd)
{
	// get the parent window client area 
	RECT rc;
	GetClientRect(hWnd, &rc);

	// create the list view
	g_hListView = CreateWindow(WC_LISTVIEW, L"",
		WS_CHILD | SBS_VERT | WS_VISIBLE | LVS_REPORT,
		0, 0, rc.right - rc.left, rc.bottom - rc.top, hWnd, 0, g_hInst, 0);

	// set full-row-select mode
	ListView_SetExtendedListViewStyle(g_hListView, LVS_EX_FULLROWSELECT);

	// set up the columns
	LVCOLUMN lvc;
	lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
	lvc.iSubItem = 0;
	auto LoadCol = [&lvc](int width, int fmt, int txtResId)
	{
		// load the column title
		TCHAR buf[128];
		LoadString(g_hInst, txtResId, buf, countof(buf));
		lvc.pszText = buf;

		// set up the item
		lvc.cx = width;
		lvc.fmt = fmt;
		ListView_InsertColumn(g_hListView, lvc.iSubItem, &lvc);

		// advance the column counter and return the current column index
		return lvc.iSubItem++;
	};

	g_lvNameCol = LoadCol(256, LVCFMT_LEFT, IDS_COL_PROGRAM);
	g_lvPidCol = LoadCol(48, LVCFMT_RIGHT, IDS_COL_PID);
	g_lvStatusCol = LoadCol(80, LVCFMT_CENTER, IDS_COL_STATUS);
	g_lvTypeCol = LoadCol(80, LVCFMT_CENTER, IDS_COL_TYPE);
	g_lvAffCol = LoadCol(128, LVCFMT_CENTER, IDS_COL_AFFINITY);
	g_lvStartedCol = LoadCol(128, LVCFMT_CENTER, IDS_COL_STARTED);
}

ListViewData* ListViewData::FromRow(int iRow)
{
	// get the item data
	ListViewData* data = 0;
	if (iRow >= 0)
	{
		LVITEM item;
		item.mask = LVIF_PARAM;
		item.iItem = iRow;
		ListView_GetItem(g_hListView, &item);
		data = (ListViewData*)item.lParam;
	}

	// return the result
	return data;
}

ListViewData* ListViewData::GetSelectedRow(int& iRow)
{
	// get the current list box selection
	iRow = ListView_GetNextItem(g_hListView, -1, LVNI_SELECTED);
	return FromRow(iRow);
}

// Get the path to a file in our program folder
void GetAppFilePath(TCHAR* buf, const TCHAR* fname)
{
	// get the full filename path of our running .exe
	TCHAR exe[MAX_PATH];
	GetModuleFileName(g_hInst, exe, countof(exe));

	// remove the file spec to get the path only
	PathRemoveFileSpec(exe);

	// build the full path as <exe folder>\filename
	PathCombine(buf, exe, fname);
}

// Load the process types
void LoadProcessTypes()
{
	// get the type file name
	TCHAR fname[MAX_PATH];
	GetAppFilePath(fname, _T("AffinityTypes.txt"));

	// read the type file
	TCHAR buf[512];
	FILE* fp = 0;
	if (_tfopen_s(&fp, fname, _T("r")) == 0)
	{
		// file exists - read it
		for (;;)
		{
			// read the next line
			if (_fgetts(buf, countof(buf), fp) == 0)
				break;

			// skip blank lines and comments
			TCHAR* p;
			for (p = buf; _istspace(*p); ++p);
			if (*p == '\n' || *p == 0 || *p == '#')
				continue;

			// find the end of the name
			const TCHAR* name = p;
			for (; *p != ':' && *p != 0 && *p != '\n'; ++p);

			// if it's not well formed, ignore the line
			if (*p != ':')
				continue;

			// null-terminate the name
			*p++ = 0;

			// parse the 64-bit hex affinity mask
			UINT64 aff = ~(UINT64)0;
			_stscanf_s(p, _T("%I64x"), &aff);

			// add the item
			g_procTypes.emplace_back(name, (DWORD_PTR)aff);
		}

		// done with the file
		fclose(fp);
	}

	// If we didn't load any types at all, add the standard "Normal"
	// type, with CPU #0 affinity, as the default
	if (g_procTypes.size() == 0)
	{
		LoadString(g_hInst, IDS_NORMAL, buf, countof(buf));
		g_procTypes.emplace_back(buf, 1);
	}

	// If we didn't load at least one custom type, add a basic 
	// "Pinball" type, with affinity for all CPUs except #0
	if (g_procTypes.size() == 1)
	{
		LoadString(g_hInst, IDS_PINBALL, buf, countof(buf));
		g_procTypes.emplace_back(buf, ~(DWORD_PTR)1);
	}
}

void LoadConfig()
{
	// try opening the config file
	FILE* fp;
	if (_tfopen_s(&fp, g_szConfigFile, _T("r")) == 0)
	{
		// read it
		for (;;)
		{
			// read a line
			TCHAR buf[512];
			if (_fgetts(buf, countof(buf), fp) == 0)
				break;

			// remove the trailing '\n'
			size_t l = _tcslen(buf);
			if (l > 0 && buf[l - 1] == '\n')
				buf[l - 1] = '\0';

			// find the ':' delimiter at the end of the name
			TCHAR* p;
			for (p = buf; *p != 0 && *p != ':'; ++p);

			// skip ill-formed lines
			if (p == buf || *p == 0)
				continue;

			// null-terminate the name
			*p++ = 0;

			// Search for the affinity type by name.  If we don't find a match,
			// use the first non-default type.
			int iType = 1;
			for (int i = 0; i < (int)g_procTypes.size(); ++i)
			{
				if (_tcscmp(g_procTypes[i].name.c_str(), p) == 0)
				{
					iType = i;
					break;
				}
			}

			// get the lowercase version of the name as the sort key
			TSTRING key = buf;
			std::transform(key.begin(), key.end(), key.begin(), ::_totlower);

			// add the entry
			g_savedProcs.emplace(
				std::piecewise_construct,
				std::forward_as_tuple(key.c_str()),
				std::forward_as_tuple(buf, iType));
		}

		// done with the file
		fclose(fp);
	}
}

bool SaveConfig()
{
	// try opening the config file
	FILE* fp;
	if (_tfopen_s(&fp, g_szConfigFile, _T("w")) != 0)
		return false;

	// write all of the saved process entries
	for (auto s : g_savedProcs)
	{
		SavedProc& sp = s.second;
		_ftprintf(fp, _T("%s:%s\n"),
			sp.name.c_str(),
			g_procTypes[sp.iType].name.c_str());
	}

	// done with the file
	fclose(fp);

	// success
	return true;
}

// Set a process affinity
void UpdateAffinity(DWORD pid, int iType, DWORD_PTR& origAffinity, DWORD_PTR& updatedAffinity, DWORD_PTR& sysAffinityMask)
{
	// Assume that we won't be able to retrieve the old affinity or
	// set a new affinity
	origAffinity = 0;
	updatedAffinity = 0;
	sysAffinityMask = 0;

	if (iType >= 0)
	{
		// figure the proposed new affinity from the new type
		DWORD_PTR proposedAffinityMask = g_procTypes[iType].affinityMask;

		// If the process ID is non-zero, try opening the process.  Process 0
		// is the special "System" process and can't be manipulated, so don't
		// even try.  We also can't set the affinity if the new mask is zero,
		// as we need at least one processor.
		if (pid != 0 && proposedAffinityMask != 0)
		{
			// try opening the process
			HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
			if (hProc != 0)
			{
				// got it - get the original affinity
				DWORD_PTR curAffinityMask;
				if (GetProcessAffinityMask(hProc, &curAffinityMask, &sysAffinityMask))
				{
					// Success - set the new affinity mask, masking out bits that
					// are invalid in the system mask.
					proposedAffinityMask &= sysAffinityMask;
					if (proposedAffinityMask != 0)
					{
						if (SetProcessAffinityMask(hProc, proposedAffinityMask))
						{
							// Success - remember the original and updated
							// affinity mask for the process list
							origAffinity = curAffinityMask;
							updatedAffinity = proposedAffinityMask;
						}
					}
				}

				// done with the process handle
				CloseHandle(hProc);
			}
		}
	}
	else
	{
		if (pid != 0)
		{
			// try opening the process
			HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
			if (hProc != 0)
			{
				// got it - get the original affinity
				DWORD_PTR curAffinityMask;
				if (GetProcessAffinityMask(hProc, &curAffinityMask, &sysAffinityMask))
				{
					curAffinityMask = sysAffinityMask;
				}
			}
		}
	}
}

// Update running processes for a new or modified SavedProc entry.
// This scans the running process list and changes the affinities for
// any running instances to match the saved affinity.
void UpdateRunningProcesses(SavedProc& saved)
{
	// reset the saved item's counter
	saved.numInstances = 0;

	// scan the process list
	for (auto& proc : g_curProcList)
	{
		// check for a match to our name
		if (proc.second.key == saved.key)
		{
			// count it
			saved.numInstances++;

			// it's a match - update its affinity
			DWORD_PTR oldAffinity, newAffinity, sysAffinity;
			UpdateAffinity(proc.second.pid, saved.iType, oldAffinity, newAffinity, sysAffinity);

			// If we succesfully set a new affinity, update the list entry
			if (newAffinity != 0)
			{
				// If the entry already had an affinity stored, we've modified
				// this process before, so DON'T update the original affinity:
				// we want to restore the original on exit, not just undo one
				// change.  If it doesn't have a stored affinity, though, it
				// means that we've never changed it before, so the old value
				// on this change is actually the original we want to restore.
				if (proc.second.newAffinity == 0)
					proc.second.origAffinity = oldAffinity;

				// store the new affinity and new system affinity values
				proc.second.newAffinity = newAffinity;
				proc.second.sysAffinity = sysAffinity;

				// mark the process list entry as dirty so that we update the
				// UI on the next refresh pass
				proc.second.dirty = true;
			}
		}
	}
}

void AddSavedProc(const TCHAR* name, int iType)
{
	// generate the key - the lowercase version of the name
	TSTRING key = name;
	std::transform(key.begin(), key.end(), key.begin(), ::_totlower);

	// if the program is already in the list, skip it
	if (g_savedProcs.find(key) != g_savedProcs.end())
		return;

	// add a new entry
	auto it = g_savedProcs.emplace(
		std::piecewise_construct,
		std::forward_as_tuple(key.c_str()),
		std::forward_as_tuple(name, iType));

	// update any running processes
	UpdateRunningProcesses(it.first->second);
}

void PopulateProcTypeMenu(HMENU hMenu)
{
	// find the "Normal" process, which is always pre-populated
	HMENU hMenuSub = FindParentMenu(hMenu, ID_SETTYPE_NORMAL);
	if (hMenuSub != 0)
	{
		// get the position of the item in its menu
		UINT pos = GetMenuPosFromID(hMenuSub, ID_SETTYPE_NORMAL);

		// add each type after Normal
		for (size_t i = 1; i < g_procTypes.size(); ++i)
		{
			MENUITEMINFO mii;
			mii.cbSize = sizeof(mii);
			mii.fMask = MIIM_STRING | MIIM_FTYPE | MIIM_ID;
			mii.dwTypeData = (TCHAR*)g_procTypes[i].name.c_str();
			mii.wID = ID_SETTYPE_NORMAL + (UINT)i;
			mii.fType = MFT_STRING;
			InsertMenuItem(hMenuSub, pos + (UINT)i, TRUE, &mii);
		}
	}
}


// Current sort column and direction
int g_sortCol = -1;
int g_sortDir = 0;

// list sort callback
int CALLBACK ListSortCb(LPARAM la, LPARAM lb, LPARAM)
{
	// get the items
	ListViewData* a = (ListViewData*)la;
	ListViewData* b = (ListViewData*)lb;

	// do the appropriate sort
	int ret = 0;
	if (g_sortCol == g_lvNameCol)
	{
		// compare by sort key
		ret = _tcscmp(a->key.c_str(), b->key.c_str());

		// sort secondarily by PID
		if (ret == 0)
			ret = a->effPid - b->effPid;
	}
	else if (g_sortCol == g_lvPidCol)
	{
		// compare by PID
		ret = a->effPid - b->effPid;
	}
	else if (g_sortCol == g_lvStatusCol)
	{
		// compare by status, with non-running saved items to the top
		int sa = a->saved != 0 ? 0 : 1;
		int sb = b->saved != 0 ? 0 : 1;
		ret = sa - sb;
	}
	else if (g_sortCol == g_lvTypeCol)
	{
		// compare by type, with pinball items to the top
		int sa = a->iType == 0 ? 65535 : a->iType;
		int sb = b->iType == 0 ? 65535 : b->iType;
		ret = sa - sb;
	}
	else if (g_sortCol == g_lvStartedCol)
	{
		ret = (int)(a->startTime.dwHighDateTime - b->startTime.dwHighDateTime);
		if (ret == 0)
			ret = (int)(a->startTime.dwLowDateTime - b->startTime.dwLowDateTime);
	}

	// if we didn't find a distinction based on the sort
	// column, sort by name, then by PID
	if (ret == 0)
		ret = _tcscmp(a->key.c_str(), b->key.c_str());
	if (ret == 0)
		ret = a->effPid - b->effPid;

	// apply the sorting direction
	ret *= g_sortDir;

	// return the result
	return ret;
}

// Sort the list view by column.  dir = 1 to sort ascending, -1 to
// sort descending, and 0 for the default order.  The default is 
// ascending if sorting on a new column, or the reverse of the
// current order if sorting on the current column.
void SortByCol(int col, int dir)
{
	// if no column was specified, use the current column
	if (col < 0)
	{
		col = g_sortCol;
		dir = g_sortDir;
	}

	// do nothing if the initial sort hasn't been set up yet
	if (col < 0)
		return;

	// If the caller specified the default direction, figure the
	// actual direction: ascending if sorting on a new column, or
	// the reverse of the current direction if sorting on the same
	// column we were previously sorting on.
	if (dir == 0)
		dir = col == g_sortCol ? -g_sortDir : 1;

	// get the header control
	HWND hdr = ListView_GetHeader(g_hListView);

	// remove the sorting arrow from the old column
	HDITEM item;
	item.mask = HDI_FORMAT;
	Header_GetItem(hdr, g_sortCol, &item);
	item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
	Header_SetItem(hdr, g_sortCol, &item);

	// set the new parameters
	g_sortCol = col;
	g_sortDir = dir;

	// set the sort arrow on the new column
	Header_GetItem(hdr, g_sortCol, &item);
	item.fmt |= dir > 0 ? HDF_SORTUP : HDF_SORTDOWN;
	Header_SetItem(hdr, g_sortCol, &item);

	// set the sort function 
	ListView_SortItems(g_hListView, ListSortCb, 0);
}

// Format a CPU affinity mask for display in the list view.
// The buffer must be at least 65 charaters long.
void FormatAffinityMask(TCHAR* buf, size_t bufcnt, DWORD_PTR mask, DWORD_PTR sysMask)
{
	if (mask == 0)
	{
		// 0 means we were unable to set the new affinity mask
		_tcscpy_s(buf, bufcnt, _T("Unable/Default"));
	}
	else
	{
		// generate the mask
		TCHAR* p = buf;
		DWORD_PTR bit = 1;
		for (int i = 0; i < 64 && (sysMask & bit) != 0 && bufcnt > 1; ++i, bit <<= 1, --bufcnt)
			*p++ = (mask & bit) != 0 ? '+' : '-';
		*p = 0;
	}
}



// Update the process list.  This is called on a timer every couple
// of seconds, and after we make changes to our internal lists.  This
// scans the current system process list, compares it to our internal
// list, and makes any necessary changes.  If we find any new processes,
// we set their affinities to match our settings.  We also update the
// UI list, unless the UI window is minimized, in which case we let it
// stay out of sync to minimize the performance impact when we're
// running the background.
void UpdateProcessList()
{
	// Update iteration counter.  We use this to identify entries
	// in the old list that are no longer in the running process
	// list.  On each update, we increment this static count; we
	// then set each entry in the old process list to the current
	// counter as we scan the new list.  We finally make a pass
	// over the old list to find any entries that *aren't* set to
	// the new counter.  Any that aren't are old entires that are
	// no longer running, so we can delete them.
	static DWORD iterCount = 0;
	++iterCount;

	// Record changes to the UI list statically.  We can make 
	// changes on multiple passes without updating the UI list,
	// since we defer UI list updates while the window is hidden.
	// We'll set this to true any time we make a change that requires
	// re-sorting the list, and we'll clear it when we actually do
	// the sorting.
	static bool changed = false;

	// Note if the UI is visible.  If it's hidden, we skip certain UI 
	// updates, to minimize the system performance impact when running 
	// in the background.  We'll bring the UI up to date when we re-show
	// the window.
	bool uiVisible = !IsIconic(g_hWnd) && IsWindowVisible(g_hWnd);

	// get the active process list
	std::list<ProcessDesc> procList;
	GetProcessList(procList);

	// scan the new process list
	for (auto const& p : procList)
	{
		// Look for an existing entry in the old list.  Make sure it 
		// matches both the PID and the process name.  We match both
		// because Windows can recycle a PID after a process terminates.
		auto it = g_curProcList.find(p.pid);
		if (it != g_curProcList.end() && it->second.name == p.name)
		{
			// this process is already in our list - flag it as updated
			it->second.updated = iterCount;
		}
		else
		{
			// There's no existing entry, so this is the first time we're seen
			// this process.  Check for a saved process entry, to see if there's
			// a custom affinity mask for this process.
			auto itsaved = g_savedProcs.find(p.key);
			SavedProc* saved = itsaved == g_savedProcs.end() ? NULL : &itsaved->second;

			// Figure the affinity type: if there's a saved entry, we use its
			// affinity type, otherwise we use the default ("normal") affinity.
			int iType = saved != 0 ? saved->iType : -1;

			// query the process start time
			HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
			FILETIME startTime = { 0, 0 };
			if (hProc != NULL)
			{
				FILETIME exitTime, kernelTime, userTime;
				GetProcessTimes(hProc, &startTime, &exitTime, &kernelTime, &userTime);
				CloseHandle(hProc);
			}

			// update the affinity
			DWORD_PTR origAffinity, updatedAffinity, sysAffinity;
			UpdateAffinity(p.pid, iType, origAffinity, updatedAffinity, sysAffinity);

			// add the process list entry
			auto itproc = g_curProcList.emplace(
				std::piecewise_construct,
				std::forward_as_tuple(p.pid),
				std::forward_as_tuple(p.pid, p.name.c_str(), p.key.c_str(), iterCount,
					origAffinity, updatedAffinity, sysAffinity, startTime));

			// create the item descriptor
			ListViewData* data = new ListViewData(
				p.pid, p.name.c_str(), p.key.c_str(),
				NULL, saved != 0 ? saved->iType : 0, startTime);

			// store a pointer to the list view data in the process item
			itproc.first->second.lvd = data;

			// add it to the list view
			LVITEM item;
			item.mask = LVFIF_TEXT | LVIF_COLFMT | LVIF_PARAM;
			item.iItem = 65535;
			item.iSubItem = 0;
			item.pszText = (TCHAR*)p.name.c_str();
			item.lParam = (LPARAM)data;
			int idx = ListView_InsertItem(g_hListView, &item);

			// format the start time
			TCHAR szStartTime[128] = _T("");
			if (startTime.dwHighDateTime != 0 || startTime.dwLowDateTime != 0)
			{
				FILETIME localTime;
				SYSTEMTIME st;
				FileTimeToLocalFileTime(&startTime, &localTime);
				FileTimeToSystemTime(&localTime, &st);
				_stprintf_s(szStartTime, _T("%04d-%02d-%02d %02d:%02d"),
					st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
			}

			// add the subitems
			TCHAR pidstr[20];
			_stprintf_s(pidstr, _T("%d"), p.pid);
			ListView_SetItemText(g_hListView, idx, g_lvPidCol, pidstr);
			ListView_SetItemText(g_hListView, idx, g_lvStatusCol, g_szRunning);
			ListView_SetItemText(g_hListView, idx, g_lvStartedCol, szStartTime);

			// mark it as dirty so that we update its affinity and type on the UI update
			itproc.first->second.dirty = true;

			// If there's a saved process entry for this process name, 
			// count the new process.
			if (saved != NULL)
				saved->numInstances++;

			// note the list view change
			changed = true;
		}
	}

	// make a list of dead items
	std::list<DWORD> deadPids;
	for (auto const& p : g_curProcList)
	{
		// check if it was update do this pass - if not, the process
		// is dead, so delete our tracker object for it
		if (p.second.updated != iterCount)
		{
			// add this PID to the deletion list
			deadPids.push_back(p.second.pid);

			// if it has a saved process entry, count the deletion
			auto itsaved = g_savedProcs.find(p.second.key);
			if (itsaved != g_savedProcs.end())
				itsaved->second.numInstances--;

			// mark its list view item as deleted
			if (p.second.lvd != NULL)
				p.second.lvd->processDeleted = true;
		}
	}

	// delete dead items from the internal list
	for (auto p : deadPids)
		g_curProcList.erase(p);

	// Update the UI list, but only if the window is visible
	if (uiVisible)
	{
		LVITEM item;
		item.mask = LVIF_PARAM;
		int n = ListView_GetItemCount(g_hListView);
		for (int i = 0; i < n; )
		{
			// get this item's lParam data
			item.iItem = i;
			ListView_GetItem(g_hListView, &item);
			ListViewData* data = (ListViewData*)item.lParam;

			// This item could be either a regular entry for a running
			// process, or a placeholder for a saved item.
			//
			// If it's a saved process placeholder, and there's at least
			// one running instance of the saved process, delete the
			// placeholder item.
			//
			// Otherwise, it's a running process entry, so check to see
			// if the process is still running, and remove the entry if
			// not.
			bool del = false;
			ProcListItem* item = 0;
			if (data->saved != NULL)
			{
				// it's a saved process placeholder entry - delete it if 
				// there are any running instances
				if (data->saved->numInstances != 0)
				{
					// delete it
					del = true;

					// note that it's no longer in the list view
					data->saved->inListView = false;
				}
				else if (data->saved->dirty)
				{
					// update the type field
					ListView_SetItemText(g_hListView, i, g_lvTypeCol, (TCHAR*)(g_procTypes[data->saved->iType].name.c_str()));
					data->iType = data->saved->iType;

					// update the affinity field
					TCHAR aff[70];
					FormatAffinityMask(aff, countof(aff), g_procTypes[data->saved->iType].affinityMask, g_sysAffinityMask);
					ListView_SetItemText(g_hListView, i, g_lvAffCol, aff);

					// mark it as clean
					data->saved->dirty = false;

					// note the change to the list
					changed = true;
				}
			}
			else
			{
				// it's a regular process entry - look up its process table entry
				auto it = g_curProcList.find(data->pid);
				if (it == g_curProcList.end())
				{
					// no process table entry - it's a dead process; delete the UI item
					del = true;
				}
				else
				{
					// note its process list item
					item = &it->second;
				}
			}

			// delete the process if we decided it should go
			if (del)
			{
				// delete it
				ListView_DeleteItem(g_hListView, i);
				--n;

				// delete the associated data object
				delete data;

				// note the list view change
				changed = true;
			}
			else
			{
				// keeping - check if it's dirty
				if (item != 0 && item->dirty)
				{
					// It's dirty - update the list display with the new affinity
					// type and CPU affinity mask

					// format the new affinity mask
					TCHAR aff[70];
					FormatAffinityMask(aff, countof(aff), item->newAffinity, item->sysAffinity);

					// update the affinity column
					ListView_SetItemText(g_hListView, i, g_lvAffCol, aff);

					// update the type column
					auto itsaved = g_savedProcs.find(item->key);
					SavedProc* saved = itsaved == g_savedProcs.end() ? NULL : &itsaved->second;
					int iType = saved != 0 ? saved->iType : 0;
					ListView_SetItemText(g_hListView, i, g_lvTypeCol, (TCHAR*)(g_procTypes[iType].name.c_str()));

					// update the list view data with the type
					ListViewData* lvd = ListViewData::FromRow(i);
					lvd->iType = iType;

					// mark it as clean
					item->dirty = false;

					// note the list view change
					changed = true;
				}

				// advance to the next item
				++i;
			}
		}

		// add placeholder items for any saved process entries that
		// have zero active processes and aren't already in the list
		for (auto& s : g_savedProcs)
		{
			SavedProc& sp = s.second;
			if (sp.numInstances == 0 && !sp.inListView)
			{
				// create the item descriptor
				ListViewData* data = new ListViewData(0, sp.name.c_str(), sp.key.c_str(), &sp, sp.iType, { 0, 0 });

				// add it to the list view
				LVITEM item;
				item.mask = LVFIF_TEXT | LVIF_COLFMT | LVIF_PARAM;
				item.iItem = 65535;
				item.iSubItem = 0;
				item.pszText = (TCHAR*)sp.name.c_str();
				item.lParam = (LPARAM)data;
				int idx = ListView_InsertItem(g_hListView, &item);

				// format the saved item's affinity mask
				TCHAR aff[70];
				FormatAffinityMask(aff, countof(aff), g_procTypes[sp.iType].affinityMask, g_sysAffinityMask);

				// add the subitems
				ListView_SetItemText(g_hListView, idx, g_lvPidCol, (TCHAR*)_T(""));
				ListView_SetItemText(g_hListView, idx, g_lvStatusCol, (TCHAR*)_T(""));
				ListView_SetItemText(g_hListView, idx, g_lvTypeCol, (TCHAR*)g_procTypes[sp.iType].name.c_str());
				ListView_SetItemText(g_hListView, idx, g_lvAffCol, aff);

				// it's now in the list
				sp.inListView = true;

				// note the list view change
				changed = true;
			}
		}

		// re-sort the list view if anything changed
		if (changed)
		{
			// do the sort
			SortByCol(-1, 0);

			// the UI is in sync with the internal list now
			changed = false;
		}
	}
}

// Restore original process affinities
void RestoreOriginalAffinities()
{
	for (auto const& pair : g_curProcList)
	{
		// get the process list item
		const ProcListItem& proc = pair.second;

		// skip process #0 - it's the system process and can't be manipulated
		if (proc.pid == 0)
			continue;

		// skip processes whose affinities we were unable to change in the
		// first place, indicated by a zero affinity mask
		if (proc.origAffinity == 0)
			continue;

		// try to open the process
		bool ok = false;
		HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, proc.pid);
		if (h != NULL)
		{
			// set the original affinity mask
			if (SetProcessAffinityMask(h, proc.origAffinity))
				ok = true;

			// done with the handle
			CloseHandle(h);
		}

		// if we didn't restore the affinity, flag it
		if (!ok)
		{
			DWORD err = GetLastError();
			LogError(_T("Unable to restore affinity for PID %ld (%s), Windows error %ld"),
				(long)proc.pid, proc.name.c_str(), (long)err);
		}
	}
}

// update the window position
void WindowPosChanged(HWND hWnd, WINDOWPOS* wp)
{
	// reposition the list view
	if (g_hListView != NULL)
	{
		RECT rc;
		GetClientRect(hWnd, &rc);
		SetWindowPos(g_hListView, 0, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOMOVE);
	}
}

// adjust menu command status
void AdjustMenuCommands(HMENU hmenu, int item)
{
	// set up a menu item info struct for menu state settings
	MENUITEMINFO mii;
	mii.cbSize = sizeof(mii);
	mii.fMask = MIIM_STATE;

	// get the current list box selection
	ListViewData* data = ListViewData::GetSelectedRow();

	// note if we have a modifable task selected
	bool taskOk = data != 0 && data->pid != 0;
	bool typeOk = taskOk || (data != 0 && data->saved != 0);

	// "End Task" is enabled only if there's a selection
	EnableMenuItem(hmenu, ID_ENDTASK, MF_BYCOMMAND | (taskOk ? MF_ENABLED : MF_DISABLED));

	// "Minimize" is enabled if we're not minimized
	bool minimized = IsIconic(g_hWnd);
	EnableMenuItem(hmenu, ID_TRAYMENU_MINIMIZE, minimized ? MF_DISABLED : MF_ENABLED);

	// "Restore" is the tray menu default
	mii.fState = MFS_DEFAULT | MFS_ENABLED;
	SetMenuItemInfo(hmenu, ID_TRAYMENU_SHOW, FALSE, &mii);

	// adjust the type items
	for (size_t i = 0; i < g_procTypes.size(); ++i)
	{
		// check it if it's the current type for the process; enable it
		// there's a modifiable task
		mii.fState = (data != 0 && data->iType == i ? MFS_CHECKED : MFS_UNCHECKED)
			| (typeOk ? MFS_ENABLED : MFS_DISABLED);
		SetMenuItemInfo(hmenu, ID_SETTYPE_NORMAL + (UINT)i, FALSE, &mii);
	}
}

// right-click the list view
void RightClickListView()
{
	// get the cursor position for menu tracking
	POINT cursor;
	GetCursorPos(&cursor);

	// track the menu
	TrackPopupMenu(GetSubMenu(g_hMenuListView, 0), TPM_LEFTALIGN | TPM_RIGHTBUTTON,
		cursor.x, cursor.y, 0, g_hWnd, NULL);
}

// show the tray menu
void ShowTrayMenu()
{
	// get the cursor position for menu tracking
	POINT cursor;
	GetCursorPos(&cursor);

	// track the menu
	TrackPopupMenu(GetSubMenu(g_hMenuTray, 0), TPM_LEFTALIGN | TPM_RIGHTBUTTON,
		cursor.x, cursor.y, 0, g_hWnd, NULL);
}

void EndTask()
{
	// get the selected row
	ListViewData* data = ListViewData::GetSelectedRow();

	// ignore it if we don't have a selected row, or it's process 0,
	// or this is a saved item rather than a running process
	if (data == 0 || data->pid == 0 || data->saved != 0)
		return;

	// ask for confirmation
	if (MsgBox(IDS_CONFIRM_END_TASK, MB_YESNO | MB_ICONWARNING) == IDYES)
	{
		// try opening the process
		bool ok = false;
		HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, data->pid);
		if (h != NULL)
		{
			// try terminating it
			ok = TerminateProcess(h, 0);

			// close the handle
			CloseHandle(h);
		}

		// if we succeeded, rescan the process list immediately; otherwise
		// show an error
		if (ok)
			UpdateProcessList();
		else
			ErrorBox(IDS_END_TASK_FAILED);
	}
}

// add a program interactively
void AddProgram()
{
	// load the title string
	TCHAR title[256];
	LoadString(g_hInst, IDS_FILEOPEN_TITLE, title, countof(title));

	// run the File Open dialog to select an .exe file
	OPENFILENAME of;
	static TCHAR customFilter[MAX_PATH] = _T("Programs\0*.exe\0\0");
	TCHAR file[32767] = _T("");
	TCHAR fileTitle[MAX_PATH] = _T("");
	of.lStructSize = sizeof(of);
	of.hwndOwner = g_hWnd;
	of.hInstance = g_hInst;
	of.lpstrFilter = _T("Programs\0*.exe\0\0");
	of.lpstrCustomFilter = customFilter;
	of.nMaxCustFilter = countof(customFilter);
	of.nFilterIndex = 0;
	of.lpstrFile = file;
	of.nMaxFile = countof(file);
	of.lpstrFileTitle = fileTitle;
	of.nMaxFileTitle = countof(fileTitle);
	of.lpstrInitialDir = 0;
	of.lpstrTitle = title;
	of.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_HIDEREADONLY;
	of.nFileOffset = 0;
	of.nFileExtension = 0;
	of.lpstrDefExt = _T(".exe");
	of.lCustData = 0;
	of.lpfnHook = 0;
	of.lpTemplateName = 0;
	of.pvReserved = 0;
	of.dwReserved = 0;
	of.FlagsEx = 0;
	if (GetOpenFileName(&of))
	{
		// The 'file' buffer contains the directory path, followed by a 
		// null character, followed by each selected filename, with an extra
		// null after the last entry.  We don't care about the path in this
		// case, just the filenames, so we can skip straight to the first
		// file.  Its offset in the buffer is given by of.nFileOffset.
		//
		// To keep the UI simple, set all new items to type the first non-
		// default type, at index 1.  The user can customize the settings
		// after adding the items if desired.
		for (const TCHAR* p = file + of.nFileOffset; *p != 0; p += _tcslen(p) + 1)
			AddSavedProc(p, 1);

		// Save the configuration changes.  If that fails, don't generate an
		// error, but mark the config as dirty so that we try saving again
		// at program exit.
		g_configDirty = !SaveConfig();

		// update the UI 
		UpdateProcessList();
	}
}

// Update the type setting for the selected list item
void UpdateTypeSetting(int iType)
{
	// get the selected row
	ListViewData* data = ListViewData::GetSelectedRow();

	// if we don't have a valid item, ignore the request
	if (data == 0 || (data->pid == 0 && data->saved == 0))
		return;

	// look up the existing saved item, if any
	auto it = g_savedProcs.find(data->key);
	if (it != g_savedProcs.end())
	{
		// There's an existing saved record.  If we're simply setting
		// the same type that's already set, there's nothing to do.
		SavedProc* saved = &it->second;
		if (saved->iType == iType)
			return;

		// Set the new type for the saved record
		saved->iType = iType;

		// mark it as dirty so we update the UI
		saved->dirty = true;

		// Update all processes of this type
		UpdateRunningProcesses(*saved);

		// If we just changed the type to the default (type 0), remove
		// the saved entry.  Saved entries are only needed for non-default
		// items.
		if (iType == 0)
			g_savedProcs.erase(it);
	}
	else
	{
		// There's no saved process entry for this program.  If we're
		// setting its type to the default (type 0), there's nothing to do, 
		// since that's already the type for anything not in the saved list.
		if (iType == 0)
			return;

		// We're setting a non-default type for a running program that
		// has no saved entry.  This means that we're implicitly creating
		// a saved entry for the program.  Add it.
		auto it = g_savedProcs.emplace(
			std::piecewise_construct,
			std::forward_as_tuple(data->key.c_str()),
			std::forward_as_tuple(data->name.c_str(), iType));

		// update running processes for the change
		UpdateRunningProcesses(it.first->second);
	}

	// Save the configuration changes.  If that fails, don't generate an
	// error, but mark the config as dirty so that we try saving again
	// at program exit.
	g_configDirty = !SaveConfig();

	// update the UI 
	UpdateProcessList();
}

// Main window proc
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_CREATE:
		// create the list view
		CreateListView(hWnd);

		// populate the process type drop menu
		PopulateProcTypeMenu(GetMenu(hWnd));

		// initialize the process list
		UpdateProcessList();

		// sort by program type
		SortByCol(g_lvTypeCol, 1);

		// set up our process update polling timer
		SetTimer(hWnd, TIMER_UPDATE, TIMER_UPDATE_TIMEOUT, NULL);

		// create our system tray icon
		CreateSysTrayIcon(hWnd);
		break;

	case WM_DESTROY:
		// delete the system tray icon
		RemoveSysTrayIcon(hWnd);

		// terminate the application
		PostQuitMessage(0);
		return 0;

	case WM_SHOWWINDOW:
		// if showing the window, do an immediate UI update
		if (wParam)
			PostMessage(hWnd, PAMSG_UPDATE, 0, 0);
		break;

	case WM_TIMER:
		switch (wParam)
		{
		case TIMER_UPDATE:
			// period process update timer
			UpdateProcessList();
			return 0;

		case TIMER_MINIMIZE:
			// The window was minimized a couple of seconds ago.  We want
			// to effectively minimize to the system tray, so hide the normal
			// task bar icon by hiding the window.  Only do this if the window
			// is actually still minimized, as the user could have restored it
			// while the minimizing animation was taking place.
			if (IsIconic(hWnd))
				ShowWindow(hWnd, SW_HIDE);

			// this is a one-shot timer, so we're done with it now
			KillTimer(hWnd, wParam);
			return 0;
		}
		break;

	case WM_WINDOWPOSCHANGED:
		WindowPosChanged(hWnd, (WINDOWPOS*)lParam);
		break;

	case WM_INITMENUPOPUP:
		AdjustMenuCommands((HMENU)wParam, LOWORD(lParam));
		break;

	case WM_SYSCOMMAND:
		// If we're restoring or maximizing from minimized or hidden,
		// update the UI list after we're back.  We skip certain UI list
		// updates when the window is minimized or hidden to reduce the
		// performance impact when running in the background, so we need
		// to bring things up to date when we re-show the window.
		if ((wParam == SC_RESTORE || wParam == SC_MAXIMIZE)
			&& (IsIconic(hWnd) || !IsWindowVisible(hWnd)))
		{
			// schedule an update
			PostMessage(hWnd, PAMSG_UPDATE, 0, 0);

			// kill any minimize-and-hide timer
			KillTimer(hWnd, TIMER_MINIMIZE);
		}

		// If we're minimizing the window, let it minimize normally,
		// but set a timer to hide it in a few seconds so that it
		// effectively minimizes to the tray.  The delay allows the
		// normal system animation effect on minimize to finish before
		// we hide the window; hiding it first hides the animation.
		if (wParam == SC_MINIMIZE)
			SetTimer(hWnd, TIMER_MINIMIZE, 500, 0);

		// do the normal system work
		return DefWindowProc(hWnd, message, wParam, lParam);

	case PAMSG_UPDATE:
		// explicitly update the UI list
		UpdateProcessList();
		return 0;

	case PAMSG_TRAY:
		// tray icon event
		switch (lParam)
		{
		case WM_LBUTTONUP:
			// if it's hidden, show it
			if (!IsWindowVisible(hWnd))
				ShowWindow(hWnd, SW_SHOWMINIMIZED);

			// bring it to the foreground
			SetForegroundWindow(hWnd);

			// if it's minimized, restore it
			if (IsIconic(hWnd))
				PostMessage(hWnd, WM_SYSCOMMAND, SC_RESTORE, 0);

			return 0;

		case WM_LBUTTONDBLCLK:
			break;

		case WM_RBUTTONUP:
			ShowTrayMenu();
			return 0;
		}
		break;

	case WM_COMMAND:
	{
		int wmId = LOWORD(wParam);
		switch (wmId)
		{
		case IDM_ABOUT:
			DialogBox(g_hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
			return 0;

		case IDM_EXIT:
		case ID_TRAYMENU_EXIT:
			DestroyWindow(hWnd);
			return 0;

		case ID_ADDPROGRAM:
			AddProgram();
			return 0;

		case ID_TRAYMENU_MINIMIZE:
			SendMessage(hWnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
			return 0;

		case ID_TRAYMENU_SHOW:
			SendMessage(hWnd, PAMSG_TRAY, 0, WM_LBUTTONUP);
			break;

		case ID_ENDTASK:
			EndTask();
			return 0;

		default:
			// check for affinity changes
			if (wmId >= ID_SETTYPE_NORMAL && wmId <= ID_SETTYPE_LAST)
			{
				UpdateTypeSetting(wmId - ID_SETTYPE_NORMAL);
				return 0;
			}

			// process to the default handling
			break;
		}
	}
	break;

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hWnd, &ps);
		EndPaint(hWnd, &ps);
	}
	return 0;

	case WM_NOTIFY:
	{
		NMHDR* nmh = (NMHDR*)lParam;
		if (nmh->hwndFrom == g_hListView)
		{
			NMLISTVIEW* nmlv = (NMLISTVIEW*)lParam;
			switch (nmh->code)
			{
			case LVN_COLUMNCLICK:
				SortByCol(nmlv->iSubItem, 0);
				break;

			case NM_RCLICK:
				RightClickListView();
				break;
			}
		}
	}
	break;

	default:
		break;
	}

	// do the default handling
	return DefWindowProc(hWnd, message, wParam, lParam);
}

// About Box dialog proc
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(lParam);

	switch (message)
	{
	case WM_INITDIALOG:
		// initialize the dialog
	{

		// machine architecture ID for display
#ifdef _M_X64
#define VSN_MACHINE _T("x64")
#elif _M_IX86
#define VSN_MACHINE _T("x86")
#else
#error Unknown architecture - add a suitable test here
#endif

		// Set the help text (it's too long for the dialog resource
			// template, so we have to set it manually here instead)
		TCHAR buf[1024];
		LoadString(g_hInst, IDS_HELP_TEXT, buf, countof(buf));
		SetDlgItemText(hDlg, IDC_TXT_HELP, buf);

		// set the version string
		_stprintf_s(buf, _T("PinAffinity, version %d.%d (Build %d, %hs, %s)"),
			VSN_MAJOR, VSN_MINOR, VSN_BUILD_NUMBER, VSN_BUILD_DATE, VSN_MACHINE);
		SetDlgItemText(hDlg, IDC_TXT_VERSIONINFO, buf);

		// set the copyright string
		_stprintf_s(buf, _T("Copyright %s%d, Michael J Roberts | MIT License"),
			VSN_BUILD_YEAR != 2018 ? _T("2018-") : _T(""), VSN_BUILD_YEAR);
		SetDlgItemText(hDlg, IDC_TXT_COPYRIGHT, buf);
	}
	return (INT_PTR)TRUE;

	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
		{
			EndDialog(hDlg, LOWORD(wParam));
			return (INT_PTR)TRUE;
		}
		break;
	}

	return (INT_PTR)FALSE;
}

