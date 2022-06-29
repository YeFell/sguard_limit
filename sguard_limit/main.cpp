// x64 SGUARD限制器，适用于各种腾讯游戏
// H3d9, 写于2021.2.5晚。
#include <Windows.h>
#include <thread>
#include "resource.h"
#include "wndproc.h"
#include "win32utility.h"
#include "config.h"
#include "kdriver.h"
#include "limitcore.h"
#include "tracecore.h"
#include "mempatch.h"


KernelDriver&           driver                  = KernelDriver::getInstance();
win32SystemManager&     systemMgr               = win32SystemManager::getInstance();
ConfigManager&          configMgr               = ConfigManager::getInstance();
LimitManager&           limitMgr                = LimitManager::getInstance();
TraceManager&           traceMgr                = TraceManager::getInstance();
PatchManager&           patchMgr                = PatchManager::getInstance();

volatile bool           g_HijackThreadWaiting   = true;
volatile DWORD          g_Mode                  = 2;      // 0: lim  1: lock  2: patch

volatile bool           g_KillAceLoader         = true;


static void HijackThreadWorker() {
	
	systemMgr.log("hijack thread: created.");

	win32ThreadManager threadMgr;

	while (1) {

		// scan per 5 seconds when idle; 
		// if process is found, trap into usr-selected mode.
		if (threadMgr.getTargetPid()) {

			systemMgr.log("hijack thread: pid found.");

			// launch clean thread to kill GameLoader at appropriate time.
			if (g_KillAceLoader) {
				systemMgr.raiseCleanThread();
			}

			// select mode.
			if (g_Mode == 0 && limitMgr.limitEnabled) {
				g_HijackThreadWaiting = false;
				limitMgr.hijack();
				g_HijackThreadWaiting = true;
			}
			if (g_Mode == 1 && traceMgr.lockEnabled) {
				g_HijackThreadWaiting = false;
				traceMgr.chase();
				g_HijackThreadWaiting = true;
			}
			if (g_Mode == 2 && patchMgr.patchEnabled) {
				g_HijackThreadWaiting = false;
				patchMgr.patch();
				g_HijackThreadWaiting = true;
			}
		}

		Sleep(3000); // call sys schedule | no target found, wait.
		// note: volatiles are written by single thread and read by multi thread. no need to sync.
	}
}


INT WINAPI WinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPSTR lpCmdLine,
	_In_ int nShowCmd) {

	bool status;


	// initialize system module: 
	// setup dpi and raise privilege (must do first)
	// init system manager (check sington, get path and os version, init log subsystem)
	// init win32 gui (create window for callback, create tray)

	systemMgr.setupProcessDpi();

	status =
	systemMgr.enableDebugPrivilege();

	if (!status) {
		return -1;
	}

	status =
	systemMgr.systemInit(hInstance);

	if (!status) {
		return -1;
	}

	status =
	systemMgr.createWindow(WndProc, IDI_ICON1);
	
	if (!status) {
		return -1;
	}

	systemMgr.createTray(WM_TRAYACTIVATE);


	// initialize configuration module:
	// load all user options from given path.
	// after load, if modules needs to modify some config, just change and writeconfig().

	configMgr.init(systemMgr.getProfileDir());

	status =
	configMgr.loadConfig();

	if (!status) {
		MessageBox(0,
			"【更新说明】\n\n"
			" 内存补丁 " MEMPATCH_VERSION "：新增支持Win8/8.1。\n\n"
			"1. 新增限制DNF更新110后SG更新导致的额外占用CPU。\n"
			"2. 增加生效速度并解决旧版某些情况下限制不生效的问题。\n"
			"3. 加了一些黑魔法 :)\n\n\n"
			
			"【重要提示】\n\n"
			"1. 本工具是免费软件，任何出售本工具的人都是骗子哦！\n\n"
			"2. 若你第一次使用，请务必仔细阅读说明（可在右下角托盘菜单中找到）\n"
			"   如果看了说明仍未解决你的问题，可以加群反馈：775176979",
			VERSION "  by: @H3d9", MB_OK);
	}


	// initialize kdriver module:
	// (if os supported) set registry, copy sys file, check sys version.
	
	auto DriverOptionsSelected = [&] ()->bool {
		return g_Mode == 2 || (g_Mode == 0 && limitMgr.useKernelMode);
	};


	if (systemMgr.getSystemVersion() == OSVersion::OTHERS) {

		// driver not supported on this system, don't call driver.init(). 
		// if selected related options, show panic.
		if (DriverOptionsSelected()) {
			systemMgr.panic(0, "内核驱动模块在你的操作系统上不受支持。\n"
			                   "【注】驱动模块支持win7/8/8.1/10/11。");
		}

	} else {

		status =
		driver.init(systemMgr.getProfileDir());

		// if driver init failed, and selected related options, show panic.
		if (!status && DriverOptionsSelected()) {

			// turn off related config flags and alert usr to switch options manually.
			limitMgr.useKernelMode = false;
			configMgr.writeConfig();
			systemMgr.panic(driver.errorCode, "%s", driver.errorMessage);
		}

		// if init success but is win11 latest,
		constexpr auto supportedLatestBuildNum = 22621;

		if (status && 
			systemMgr.getSystemVersion() == OSVersion::WIN_10_11 && 
			systemMgr.getSystemBuildNum() > supportedLatestBuildNum) {

			// if force enable bit not set, but user selected related options (first run default),
			// or force enable bit set, but build num not match (system updated) :
			if ((!driver.win11ForceEnable && DriverOptionsSelected()) ||
				(driver.win11ForceEnable && systemMgr.getSystemBuildNum() != driver.win11CurrentBuild)) {

				// alert user to confirm potential bsod threat.
				char buf[0x1000];
				sprintf(buf, "【！！！请仔细阅读：潜在的蓝屏风险！！！】\n\n\n"
					"当前系统版本超出内核驱动模块已确认支持的最高系统版本：\n\n"
					"已确认支持的Win11版本：10.0.%d\n"
					"当前Win11系统版本：10.0.%d\n\n\n"
					"若你启动游戏后右键菜单显示已提交，表示兼容，且可以保证下次系统更新前都没问题。\n\n"
					"若每次游戏启动时都蓝屏，表示内核驱动模块不再兼容。你可以反馈到群里。\n\n\n"
					"如果你已了解上述情况，并可以承担蓝屏风险，请点击“是”，否则请点击“否”。",
					supportedLatestBuildNum, systemMgr.getSystemBuildNum());
				
				if (IDYES == MessageBox(0, buf, "系统版本警告", MB_YESNO)) {
					driver.driverReady        = true;
					driver.win11ForceEnable   = true;
					driver.win11CurrentBuild  = systemMgr.getSystemBuildNum();
				} else {
					driver.driverReady        = false;
					driver.win11ForceEnable   = false;
					driver.win11CurrentBuild  = 0;
				}

				configMgr.writeConfig();
			}
		}

		// show error if driver not init correctly.
		if (!driver.driverReady) {
			systemMgr.panic(0, "由于驱动初始化失败，以下关联模块无法使用：\n\n"
							   "内存补丁 " MEMPATCH_VERSION "\n"
							   "内核态调度器\n");
		}
	}


	// initialize patch module:
	// get all native syscall numbers it should use.

	if (!patchMgr.init()) {
		systemMgr.panic(0, "“内存补丁 " MEMPATCH_VERSION "”模块初始化失败。\n"
		                   "查看%s\\log.txt以获得更多信息。", systemMgr.getProfileDir().c_str());
		
		// this rarely happens, disable kdriver to stop user from continue.
		driver.driverReady = false;
	}


	// create working thread:
	// using std::thread (_beginthreadex) is more safe than winapi CreateThread;
	// because we use heap and crt functions in working thread.

	auto HijackThreadCaller = [] () {
		std::thread hijackThread(HijackThreadWorker);
		hijackThread.detach();
	};

	HijackThreadCaller();


	// enter primary msg loop:
	// main thread will wait for window msgs from usr, while working thread do actual works.

	auto result =
	systemMgr.messageLoop();


	// program exit:
	// after winmain() returns, all static sington objects are destructed.

	systemMgr.removeTray();

	return (INT) result;
}