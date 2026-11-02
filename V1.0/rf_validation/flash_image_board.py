"""Flash only a factory-ID-matched image board, even with cloned probe serials.

Temporarily putting the first probe in DFU exposes the other probe to OpenOCD.
No probe firmware is updated. TI's tool requires rewriting the same original
serial to exit ROM DFU without downloading firmware; configuration is preserved.
"""
import argparse
import re
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[2]
openocd = root/'tools/openocd/bin/openocd.exe'
scripts = root/'tools/openocd/openocd/scripts'
dfu = Path('G:/sofeware/ccs/ccs/ccs_base/common/uscif/xds110/xdsdfu.exe')
ids = {'tx': 0x376111f7, 'rx': 0x219d1542}
ap = argparse.ArgumentParser()
ap.add_argument('role', choices=ids)
ap.add_argument('elf', type=Path)
ap.add_argument('--log', required=True, type=Path)
ap.add_argument('--factory-id', type=lambda value: int(value, 0),
                help='Confirmed replacement board factory ID low word')
ap.add_argument('--read', nargs='+', help='Read named uint32 debug symbols instead of flashing')
a = ap.parse_args()
if a.factory_id is not None:
    ids[a.role] = a.factory_id
elf = a.elf.resolve()
assert elf.is_file()
log = []

def run(command):
    result = subprocess.run([str(s) for s in command], cwd=root,
                            capture_output=True, timeout=90)
    output = (result.stdout+result.stderr).decode('utf-8', errors='replace')
    log.append(output)
    print(output, flush=True)
    return result.returncode, output

def ocd(command):
    return run([openocd, '-s', scripts, '-f', 'board/ti_cc13x0_launchpad.cfg',
                '-c', 'gdb_port disabled; tcl_port disabled; telnet_port disabled; adapter speed 200; '+command])

def selected():
    rc, output = ocd('init; echo FACTORY=[read_memory 0x500012f0 32 2]; shutdown')
    match = re.search(r'FACTORY=(0x[0-9a-f]+) (0x[0-9a-f]+)', output, re.I)
    return None if rc or not match else tuple(int(v,16) for v in match.groups())

switched = False
original_serial = None
try:
    if selected() != (ids[a.role], 0x124b00):
        rc, inventory = run([dfu, '-e'])
        first = inventory.split('<<<< Device 0 >>>>')[1].split('<<<< Device 1 >>>>')[0]
        original_serial = re.search(r'Serial Num:\s+(\S+)', first).group(1)
        if not re.fullmatch(r'[A-Za-z0-9]{8}', original_serial):
            raise RuntimeError('Cannot preserve original probe serial')
        rc, _ = run([dfu, '-i', '0', '-m'])
        switched = True
        if rc or selected() != (ids[a.role], 0x124b00):
            raise RuntimeError('Cannot select expected factory ID; no flash write performed')
    relative = elf.relative_to(root).as_posix()
    guard = ('init; set chip [read_memory 0x500012f0 32 2]; echo "FLASH_ID=$chip"; '
             'if {[lindex $chip 0] != 0x%x || [lindex $chip 1] != 0x124b00} '
             '{error "Factory identity mismatch: refusing flash"}; '
             'program {%s} verify reset exit') % (ids[a.role], relative)
    if a.read:
        nm = Path('F:/stm32cube_ide/STM32CubeIDE_1.5.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.7-2018-q2-update.win32_1.5.0.202011040924/tools/bin/arm-none-eabi-nm.exe')
        symbols = subprocess.check_output([str(nm), str(elf)], text=True)
        addresses = {name: int(address,16) for address, kind, name in
                     (line.split() for line in symbols.splitlines() if len(line.split()) == 3)}
        guard = guard.split('program {')[0] + '; '.join(
            'echo "%s=[read_memory 0x%x 32 1]"' % (name, addresses[name]) for name in a.read) + '; shutdown'
    rc, output = ocd(guard)
    if rc or (not a.read and 'Verified OK' not in output):
        raise RuntimeError('Programming/verification failed')
finally:
    if switched:
        rc, inventory = run([dfu, '-e'])
        for index, block in re.findall(r'<<<< Device (\d+) >>>>(.*?)(?=<<<< Device|Found \d+ devices)', inventory, re.S):
            if re.search(r'Mode:\s+DFU', block):
                rc, restored = run([dfu, '-i', index, '-s', original_serial, '-r'])
                if rc or 'ERROR:' in restored:
                    raise RuntimeError('Probe DFU exit failed; original serial is '+original_serial)
    a.log.parent.mkdir(parents=True, exist_ok=True)
    a.log.write_text('\n'.join(log), encoding='utf-8')
