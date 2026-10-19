#!/usr/bin/env python3
# devops.py - 统一的开发/运维入口（构建 / 烧录 / 测试）—— Python 3 跨平台实现
#
# 纯 Python 3 实现，去掉对 bash 的依赖，Linux/macOS/Windows（装 Docker + Python 3）
# 均可执行：
#     python3 tools/devops.py build [选项]
#     python3 tools/devops.py test | flash | reboot | shell
#
# 构建与测试一律在**同一个 Docker 镜像**内完成，宿主机无需安装任何工具链。
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _env(name: str, default: str) -> str:
    v = os.environ.get(name)
    return v if v not in (None, "") else default


# 各值也可用同名环境变量作为默认值（选项优先）
BOARD = _env("BOARD", "stm32f103")                 # 目标板，默认 stm32f103（可简写 f103）
RTOS = _env("RTOS", "rt-thread")
GEN = _env("GEN", "Ninja")
TOOLCHAIN = _env("TOOLCHAIN", str(ROOT / "cmake/toolchain-arm-none-eabi.cmake"))
GPIO_BACKEND = _env("GPIO_BACKEND", "stm32f1")
# --rtos zephyr 时对应的 Zephyr 官方 board（主机可选择 STM32F103 系开发板）
ZEPHYR_BOARD = _env("ZEPHYR_BOARD", "stm32f103_mini")
IMG = _env("IMG", "stm32f103-toolchain:latest")
DOCKERFILE = _env("DOCKERFILE", str(ROOT / "docker/Dockerfile"))
APPLY_PATCHES = _env("APPLY_PATCHES", "1")

# BUILD_DIR/TEST_BUILD 默认派生自 BOARD；若显式给过环境变量则不再随 --board 移动。
_BUILD_DIR_ENV = _env("BUILD_DIR", "")
BUILD_DIR_SET = _BUILD_DIR_ENV != ""
BUILD_DIR = _BUILD_DIR_ENV if BUILD_DIR_SET else str(ROOT / "build" / BOARD)

USAGE = """\
用法:
  python3 tools/devops.py build [选项]         Docker 交叉编译固件（唯一构建方式）
      选项:
        --board <板名>        目标板（默认 stm32f103，可简写 f103）
        --rtos <内核>         RTOS 后端：rt-thread / zephyr / freertos（默认 rt-thread）
        --gpio-backend <名>   GPIO 实现后端（默认 stm32f1）
        --zephyr-board <名>   --rtos zephyr 对应 Zephyr 官方板（默认 stm32f103_mini）
        --host|--test|-t      额外编译 host 单测程序（宿主 gcc；仅编译，不运行）
        --clean|-c            build 前先清理本次固件构建目录（干净重建；zephyr 保留 .venv-zephyr/external，无需联网重拉模块）
  python3 tools/devops.py test                只**运行**已编译好的 host 单测（编译请先 build --host）
  python3 tools/devops.py flash [--flasher stlink|dap|isp] [--rtos rt-thread|zephyr]  烧录
        --rtos <内核>         烧哪种固件：rt-thread（默认）/ zephyr（自动选中对应镜像）
        --bin <文件>          直接指定镜像（.bin 自动按 0x08000000 烧；优先级高于 --rtos）
        --flasher isp         串口 ISP 烧录（stm32flash，无需 DAP；需 BOOT0=1，用 CH340/UART1）
  python3 tools/devops.py reboot [--flasher dap|stlink] [--rtos rt-thread|zephyr]  用调试器从主 Flash
        重启运行（绕开 DAP 软复位进 ROM 的问题，等于隔空按 RESET；重启后串口重打启动横幅）
  python3 tools/devops.py shell              进入 Docker 容器交互式 shell（同 build 环境，便于手动编译/排查）
  python3 tools/devops.py help                显示本帮助

示例:
  python3 tools/devops.py build                          # 默认板 stm32f103 + rt-thread，只编译固件
  python3 tools/devops.py build --host                   # 编译固件 + 编译 host 单测程序（不运行）
  python3 tools/devops.py test                           # 只运行已编译的 host 单测
"""


def usage(code: int = 0) -> "NoReturn":
    print(USAGE, end="")
    sys.exit(code)


def log(msg: str) -> None:
    print(msg, flush=True)


def die(msg: str, code: int = 1) -> "NoReturn":
    print(msg, file=sys.stderr, flush=True)
    sys.exit(code)


def run(cmd, **kwargs) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(cmd, check=True, **kwargs)
    except FileNotFoundError:
        die(f"[devops] 未找到命令: {cmd[0]}（请确认已安装并加入 PATH）")
    except subprocess.CalledProcessError as e:
        # 与 bash `set -e` 一致：命令失败即退出，命令自身错误信息已打印，不叠 Python traceback
        sys.exit(e.returncode)


def normalize_board(name: str) -> str:
    return "stm32f103" if name in ("f103", "stm32f103") else name


def apply_board(name: str) -> None:
    """按新的 BOARD 重算派生目录（仅当未显式设置 BUILD_DIR 时）。"""
    global BOARD, BUILD_DIR
    BOARD = normalize_board(name)
    if not BUILD_DIR_SET:
        BUILD_DIR = str(ROOT / "build" / BOARD)


def docker_user_args() -> list:
    """返回 docker run 的用户参数；Windows(Docker Desktop) 下不需要 -u。"""
    if os.name == "nt":
        return []
    return ["-u", f"{os.getuid()}:{os.getgid()}"]


def ensure_image() -> None:
    log(f"[devops] 准备镜像 {IMG}（首次较慢，之后命中缓存）")
    run(["docker", "build", "-t", IMG, "-f", DOCKERFILE, str(Path(DOCKERFILE).parent)])


def kill_openocd() -> None:
    """清掉上一次可能残留的 openocd，避免同一时刻抢同一个调试器。"""
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/IM", "openocd.exe"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        subprocess.run(["pkill", "-9", "-f", "openocd"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _docker_base() -> list:
    """docker run 公共参数：以当前用户挂载工程根目录并作为工作目录。"""
    return ["docker", "run", "--rm", *docker_user_args(),
            "-v", f"{ROOT}:{ROOT}", "-w", str(ROOT)]


# ---- 编译固件（Docker 交叉编译，arm-none-eabi-gcc）----
def do_build() -> None:
    ensure_image()
    Path(BUILD_DIR).mkdir(parents=True, exist_ok=True)
    log(f"[devops] 目标板 BOARD={BOARD}")
    log(f"[devops] 容器内编译固件：RTOS={RTOS} GPIO_BACKEND={GPIO_BACKEND}")
    log(f"[devops] 挂载 {ROOT} -> {ROOT} ，产物目录 {BUILD_DIR}")

    # -- Zephyr 走 Zephyr 自带构建体系（构建宿主是 Zephyr，不走顶层 CMakeLists）--
    if RTOS == "zephyr":
        log(f"[devops] Zephyr 构建：BOARD（Zephyr 板）={ZEPHYR_BOARD}")
        run(_docker_base() + [
            "-e", f"ZEPHYR_BASE={ROOT}/rtos/zephyr",
            "-e", f"ZEPHYR_BOARD={ZEPHYR_BOARD}",
            "-e", f"BUILD_DIR={BUILD_DIR}/zephyr",
            "-e", f"GEN={GEN}",
            IMG, "bash", "-lc", "set -e\npython3 tools/build_zephyr.py",
        ])
        log(f"[devops] 完成（{BOARD} / zephyr）。产物目录：{BUILD_DIR}/zephyr/zephyr/")
        log(f"        ELF : {BUILD_DIR}/zephyr/zephyr/zephyr.elf")
        log(f"        BIN : {BUILD_DIR}/zephyr/zephyr/zephyr.bin")
        log(f"        HEX : {BUILD_DIR}/zephyr/zephyr/zephyr.hex")
        log(f"        map : {BUILD_DIR}/zephyr/zephyr/zephyr.map")
        return

    inline = """set -e
python3 tools/apply_patches.py
cmake -S . -B "${BUILD_DIR}/rt-thread" -G "$GEN" \\
      -DBOARD="$BOARD" \\
      -DRTOS="$RTOS" \\
      -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \\
      -DGPIO_BACKEND="$GPIO_BACKEND"
cmake --build "${BUILD_DIR}/rt-thread"
"""
    run(_docker_base() + [
        "-e", f"BOARD={BOARD}",
        "-e", f"RTOS={RTOS}",
        "-e", f"GPIO_BACKEND={GPIO_BACKEND}",
        "-e", f"BUILD_DIR={BUILD_DIR}",
        "-e", f"GEN={GEN}",
        "-e", f"TOOLCHAIN={TOOLCHAIN}",
        "-e", f"APPLY_PATCHES={APPLY_PATCHES}",
        IMG, "bash", "-lc", inline,
    ])
    log(f"[devops] 完成（{BOARD}）。产物:")
    log(f"        ELF : {BUILD_DIR}/rt-thread/stm32f103（主 ELF，供 GDB）")
    log(f"        BIN : {BUILD_DIR}/rt-thread/stm32f103.bin")
    log(f"        HEX : {BUILD_DIR}/rt-thread/stm32f103.hex")


def _test_build_dir() -> str:
    return _env("TEST_BUILD", str(ROOT / "build" / BOARD / "rt-thread/tests"))


# ---- 编译 host 单测程序（宿主 gcc；仅编译，不运行）----
def do_build_test() -> None:
    ensure_image()
    tbuild = _test_build_dir()
    log(f"[devops] 编译 host 单测程序（宿主 gcc，仅编译，不运行）：{tbuild}")
    inline = """set -e
cmake -S tests/gpio -B "$TEST_BUILD" -G "$GEN"
cmake --build "$TEST_BUILD"
"""
    run(_docker_base() + [
        "-e", f"TEST_BUILD={tbuild}",
        "-e", f"GEN={GEN}",
        IMG, "bash", "-lc", inline,
    ])
    log("[devops] host 单测程序已就绪，运行请用 python3 tools/devops.py test")


# ---- 运行 host 单测（只运行 build --host 已编译好的程序，不重新编译）----
def do_test() -> None:
    tbuild = _test_build_dir()
    if not (Path(tbuild) / "test_rtthread_gpio").is_file():
        die("[devops] 尚未编译 host 单测程序，请先运行：python3 tools/devops.py build --host（或 --test）。")
    ensure_image()
    log(f"[devops] 运行 host 单测（目标板 {BOARD}，目录 {tbuild}）")
    inline = """set -e
ctest --test-dir "$TEST_BUILD" --output-on-failure
"""
    run(_docker_base() + [
        "-e", f"TEST_BUILD={tbuild}",
        IMG, "bash", "-lc", inline,
    ])
    log(f"[devops] 容器内测试完成（{BOARD}）")


# ---- build 子命令：选项解析 + 编译（可 --host/--test 顺带编译 host 单测程序）----
def build(args: list) -> None:
    global RTOS, GPIO_BACKEND, ZEPHYR_BOARD
    host = False
    clean = False
    b, r, g, zb = BOARD, RTOS, GPIO_BACKEND, ZEPHYR_BOARD
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("--host", "--test", "-t"):
            host = True
        elif a in ("--clean", "-c"):
            clean = True
        elif a == "--board":
            i += 1
            if i >= len(args):
                die("--board 需要参数")
            b = args[i]
        elif a.startswith("--board="):
            b = a.split("=", 1)[1]
        elif a == "--rtos":
            i += 1
            if i >= len(args):
                die("--rtos 需要参数")
            r = args[i]
        elif a.startswith("--rtos="):
            r = a.split("=", 1)[1]
        elif a == "--zephyr-board":
            i += 1
            if i >= len(args):
                die("--zephyr-board 需要参数")
            zb = args[i]
        elif a.startswith("--zephyr-board="):
            zb = a.split("=", 1)[1]
        elif a == "--gpio-backend":
            i += 1
            if i >= len(args):
                die("--gpio-backend 需要参数")
            g = args[i]
        elif a.startswith("--gpio-backend="):
            g = a.split("=", 1)[1]
        elif a in ("--help", "-h"):
            usage(0)
        else:
            die(f"[devops] build 未知选项: {a}（可用 --board/--rtos/--zephyr-board/--gpio-backend/--host/--clean）")
        i += 1

    apply_board(b)
    RTOS = r
    GPIO_BACKEND = g
    ZEPHYR_BOARD = zb

    # --clean：干净重建前先清掉本次固件将写入的目录（zephyr / rt-thread 各自独立清理）。
    if clean:
        if RTOS == "zephyr":
            zdir = Path(BUILD_DIR) / "zephyr"
            if zdir.is_dir():
                for p in zdir.iterdir():
                    if p.name in (".venv-zephyr", "external"):
                        continue
                    if p.is_dir() or p.is_symlink():
                        shutil.rmtree(p)
                    else:
                        p.unlink()
            log(f"[devops] 已清理 {BUILD_DIR}/zephyr 下固件构建产物（保留 .venv-zephyr 与 external/ 复用，无需联网重拉模块）")
        else:
            rdir = Path(BUILD_DIR) / "rt-thread"
            if rdir.exists():
                shutil.rmtree(rdir)
            log(f"[devops] 已清理 {BUILD_DIR}/rt-thread（含固件与 host 单测；重建自动重新 cmake）")

    do_build()
    if host:
        do_build_test()


# ---- 进入容器交互式 shell ----
# 与 build 共用同一镜像、同一挂载（工程目录 ROOT 双向挂载为工作目录），
# 但以 -it 交互方式进入，便于在容器里手动跑 cmake/arm-none-eabi-gcc 编译或排查环境。
# 注意：容器默认访问不到宿主机 USB（无 --device），烧录/探测 DAP 仍需在宿主机做。
def do_shell() -> None:
    di = os.environ.get("DOCKER_INTERACTIVE")
    if di is None or di == "":
        interactive = sys.stdin.isatty() and sys.stdout.isatty()
    else:
        interactive = di == "1"

    if not interactive:
        die("[devops] shell 需要交互式终端（-it）。请在前台终端中执行：python3 tools/devops.py shell")

    ensure_image()
    log(f"[devops] 进入容器 shell：{IMG}（工程挂载于容器内 {ROOT}）")
    log("[devops] 容器内仅用于编译/排查；烧录(DAP)请在宿主机执行。退出用 exit")
    run(["docker", "run", "--rm", "-it", *docker_user_args(),
         "-v", f"{ROOT}:{ROOT}", "-w", str(ROOT), IMG, "bash"])


# ---- 烧录（宿主机 openocd）----
# 与 build 不同，烧录必须在宿主机（能看到调试器 USB 的环境）执行。
# 用法：python3 tools/devops.py flash [--flasher stlink|dap|isp] [--bin <file>]
#   也可用环境变量 FLASHER / CONFIG / BIN / PORT / BAUD / GDB_PORT / RESET_MODE 覆盖默认。
def flash(args: list) -> None:
    flasher = _env("FLASHER", "stlink")
    # --rtos 指定烧哪种固件；缺省 rt-thread，向后兼容（老命令 flash --flasher xxx 仍烧 rt-thread）。
    rtos = _env("FLASH_RTOS", "rt-thread")
    bin_path = _env("BIN", "")

    i = 0
    while i < len(args):
        a = args[i]
        if a in ("--flasher", "-i"):
            i += 1
            if i >= len(args):
                die("--flasher 需要参数")
            flasher = args[i]
        elif a.startswith("--flasher="):
            flasher = a.split("=", 1)[1]
        elif a == "--rtos":
            i += 1
            if i >= len(args):
                die("--rtos 需要参数")
            rtos = args[i]
        elif a.startswith("--rtos="):
            rtos = a.split("=", 1)[1]
        elif a == "--bin":
            i += 1
            if i >= len(args):
                die("--bin 需要参数")
            bin_path = args[i]
        elif a.startswith("--bin="):
            bin_path = a.split("=", 1)[1]
        else:
            die(f"[devops] 未知 flash 选项: {a}")
        i += 1

    # 未显式给 bin 时，按 --rtos 推导对应镜像路径
    if not bin_path:
        rtos_bin = {
            "rt-thread": f"{BUILD_DIR}/rt-thread/stm32f103.bin",
            "rtt": f"{BUILD_DIR}/rt-thread/stm32f103.bin",
            "rtthread": f"{BUILD_DIR}/rt-thread/stm32f103.bin",
            "zephyr": f"{BUILD_DIR}/zephyr/zephyr/zephyr.bin",
            "zph": f"{BUILD_DIR}/zephyr/zephyr/zephyr.bin",
            "zeph": f"{BUILD_DIR}/zephyr/zephyr/zephyr.bin",
        }
        bin_path = rtos_bin.get(rtos)
        if bin_path is None:
            die(f"[devops] 未知 --rtos: {rtos}（支持 rt-thread / zephyr）")

    if not Path(bin_path).is_file():
        die(f"[devops] 未找到 {bin_path}，请先运行 python3 tools/devops.py build --rtos {rtos} 生成对应镜像，或用 --bin 直接指定文件。")

    # ---- 串口 ISP 烧录（stm32flash，无需 DAP）----
    if flasher == "isp":
        port = _env("PORT", "/dev/ttyUSB0")
        baud = _env("BAUD", "115200")
        if shutil.which("stm32flash") is None:
            die("[devops] 未安装 stm32flash，请：sudo apt install stm32flash")
        if not Path(port).exists():
            die(f"[devops] 未找到串口 {port}（确认 CH340 枚举为 /dev/ttyUSB0）")
        log("[devops] ISP 烧录：请确认板子 BOOT0=1（ISP 模式）并已复位。")
        log(f"[devops] {bin_path} -> {port} @ {baud}bps")
        run(["stm32flash", "-w", bin_path, "-v", "-b", baud, port])
        log("[devops] 写入并校验完成。请将 BOOT0 拨回 0，复位后固件从主 Flash 运行。")
        return

    # 纯烧录用不到 gdb server；默认禁用避免 3333 被占。需调试时设 GDB_PORT=3333。
    gdb_port = _env("GDB_PORT", "")
    gdb_args = ["-c", f"gdb_port {gdb_port}"] if gdb_port else ["-c", "gdb_port 0"]

    extra_args: list = []
    flash_auto_run = False
    if flasher in ("stlink", "st-link"):
        config = _env("CONFIG", str(ROOT / "tools/openocd-stm32f1.cfg"))
    elif flasher in ("dap", "cmsis-dap", "cmsis"):
        config = _env("CONFIG", str(ROOT / "tools/openocd-stm32f1-dap.cfg"))
        # 这颗正点原子 ATK-FS-HID CMSIS-DAP 的复位时序会把芯片带进 ROM BootLoader，
        # 默认只写 Flash+校验、不碰复位（复位交给板上 RESET 键/断电上电干净渠道）。
        if _env("RESET_MODE", "soft") == "srst":
            extra_args = ["-c", "reset_config srst_only"]
            flash_auto_run = True
    else:
        die(f"[devops] 未知烧录器: {flasher}（支持: stlink / dap / isp）")

    log(f"[devops] 烧录器={flasher}，烧录 {bin_path} ...")
    is_dap = flasher in ("dap", "cmsis-dap", "cmsis")
    if is_dap:
        if flash_auto_run:
            # RESET_MODE=srst：尝试烧完后硬件复位运行（依赖 DAP nRST 正常接通）
            log("[devops] RESET_MODE=srst：烧完后自动硬件复位运行（需 DAP nRST 接通）")
            run(["openocd", "-f", config, *extra_args, *gdb_args,
                 "-c", f"program {bin_path} 0x08000000 verify reset exit"])
            log("[devops] 烧录完成。固件已尝试自动复位运行。")
        else:
            # 默认（推荐）：只写 Flash+校验，不碰复位（DAP 的 reset 会把芯片带进 ROM BootLoader）。
            run(["openocd", "-f", config, *gdb_args,
                 "-c", f"program {bin_path} 0x08000000 verify exit"])
            log("[devops] 烧录完成(写Flash+校验OK)。固件已就绪，尚未启动。")
            log("[devops] 现在请按一下【板上 RESET 键】(或断电上电)，固件将从主 Flash")
            log("[devops] 干净启动并连续输出 heartbeat。")
    else:
        # stlink：program ... verify reset exit 烧完自动复位运行，等于 MDK Reset and Run
        run(["openocd", "-f", config, *extra_args, *gdb_args,
             "-c", f"program {bin_path} 0x08000000 verify reset exit"])
        log("[devops] 烧录完成。固件已自动复位运行。")


# ---- 用调试器强制目标从主 Flash 重启运行 ----
# 用法：python3 tools/devops.py reboot [--flasher dap|stlink] [--rtos rt-thread|zephyr] [--bin <file>]
# 用调试器强制目标从 Flash 主启动向量(Reset_Handler) 重新运行：
#   init + halt -> 把 SP/PC 设为固件 bin 文件头的初始栈指针与 Reset 向量 -> resume。
# 作用：绕开这颗 HID DAP 软复位会进 ROM BootLoader 的问题，等于“隔空按 RESET”。
def reboot(args: list) -> None:
    flasher = "dap"
    rtos = _env("FLASH_RTOS", "rt-thread")
    bin_path = ""

    i = 0
    while i < len(args):
        a = args[i]
        if a == "--flasher":
            i += 1
            if i >= len(args):
                die("--flasher 需要参数")
            flasher = args[i]
        elif a.startswith("--flasher="):
            flasher = a.split("=", 1)[1]
        elif a == "--rtos":
            i += 1
            if i >= len(args):
                die("--rtos 需要参数")
            rtos = args[i]
        elif a.startswith("--rtos="):
            rtos = a.split("=", 1)[1]
        elif a == "--bin":
            i += 1
            if i >= len(args):
                die("--bin 需要参数")
            bin_path = args[i]
        elif a.startswith("--bin="):
            bin_path = a.split("=", 1)[1]
        else:
            die(f"[devops] reboot 未知选项: {a}（--flasher dap|stlink [--rtos|--bin]）")
        i += 1

    if flasher in ("dap", "cmsis-dap"):
        config = str(ROOT / "tools/openocd-stm32f1-dap.cfg")
    elif flasher in ("stlink", "st-link"):
        config = str(ROOT / "tools/openocd-stm32f1.cfg")
    elif flasher == "isp":
        die("[devops] reboot 不支持 isp（ISP 无调试复位接口，请用板上 RESET）")
    else:
        die(f"[devops] reboot 未知 flasher: {flasher}（dap / stlink）")

    if not bin_path:
        rtos_bin = {
            "rt-thread": f"{BUILD_DIR}/rt-thread/stm32f103.bin",
            "zephyr": f"{BUILD_DIR}/zephyr/zephyr/zephyr.bin",
        }
        bin_path = rtos_bin.get(rtos)
        if bin_path is None:
            die(f"[devops] reboot 未知 --rtos: {rtos}（rt-thread / zephyr）")

    if not Path(bin_path).is_file():
        die(f"[devops] 未找到 {bin_path}，请先运行 python3 tools/devops.py build --rtos {rtos} 或用 --bin 指定。")

    # 从 bin 文件头解析初始 SP 与 Reset 向量（小端）
    with open(bin_path, "rb") as f:
        head = f.read(8)
    if len(head) < 8:
        die(f"[devops] reboot: 无法解析 {bin_path} 的向量表")
    sp, reset = struct.unpack("<II", head)

    log(f"[devops] reboot：强制从 {bin_path} 的 Reset(0x{reset:08X}) 重新运行（sp=0x{sp:08X}），多试几次以稳过 DAP 时序...")
    speeds = [100, 400, 1000]
    for attempt in range(1, 11):
        kill_openocd()  # 清掉上一次可能残留的 openocd，避免同一时刻抢同一个调试器
        time.sleep(0.4)
        spd = speeds[(attempt - 1) % len(speeds)]
        cmd = ["openocd", "-f", config,
               "-c", "gdb_port 0", "-c", f"adapter speed {spd}",
               "-c", "init", "-c", "halt",
               "-c", f"reg sp 0x{sp:08X}", "-c", f"reg pc 0x{reset:08X}",
               "-c", "resume", "-c", "shutdown"]
        out = ""
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
            out = (r.stdout or "") + (r.stderr or "")
        except subprocess.TimeoutExpired:
            out = "timeout"
        except FileNotFoundError:
            die("[devops] 未找到 openocd（flash/reboot 需宿主机安装 openocd）")

        if re.search(r"halted|resumed|running", out, re.IGNORECASE):
            log(f"[devops] 第 {attempt} 次成功(speed={spd}kHz)：已让固件从主 Flash 启动入口重跑。")
            log("[devops] 现在看串口(115200)，应能看到内核启动横幅 + [app28-...] heartbeat。")
            return

        matched = re.findall(r"Target not examined|Failed to read|Error", out, re.IGNORECASE)
        err = matched[-1] if matched else ""
        log(f"[devops] 第 {attempt} 次尝试({spd}kHz)未 halt：{err or '无错误信息'}")
        time.sleep(1)

    print("[devops] reboot: DAP 10 次均未稳定 halt，请确认 DAP 已插好（/dev/ttyACM*）后再试，或按板上 RESET。",
          file=sys.stderr)
    sys.exit(1)


# ---- 入口 ----
def main(argv: list) -> None:
    if not argv:
        usage(0)
    cmd, rest = argv[0], argv[1:]
    if cmd == "build":
        build(rest)
    elif cmd in ("test", "tests"):
        do_test()
    elif cmd == "flash":
        flash(rest)
    elif cmd == "reboot":
        reboot(rest)
    elif cmd in ("shell", "devshell", "dev"):
        do_shell()
    elif cmd in ("help", "-h", "--help"):
        usage(0)
    else:
        print(f"[devops] 未知命令: {cmd}（可用 build|test|flash|reboot|shell|help）", file=sys.stderr)
        usage(1)


if __name__ == "__main__":
    main(sys.argv[1:])