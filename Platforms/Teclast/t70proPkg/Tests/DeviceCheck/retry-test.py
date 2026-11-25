from pathlib import Path
import subprocess
m=Path('/home/builder/teclast-t70pro/Mu-Silicium');w=Path(__file__).resolve().parent
out=m.parent/'analysis/usb-device-check-20261117'
incs=['Mu_Basecore/MdePkg/Include','Mu_Basecore/MdePkg/Include/AArch64','Mu_Basecore/MdeModulePkg/Include','Mu_Basecore/MdeModulePkg/Bus/Usb/UsbMassStorageDxe']
cmd=['gcc','-std=gnu11','-O1','-g','-fshort-wchar','-DMDEPKG_NDEBUG','-DT70PRO_RAM_DIAGNOSTICS','-fsanitize=address,undefined','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','--param=asan-globals=0','-include','Base.h']+['-I'+str(m/x) for x in incs]+[str(w/'retry-test.c'),str(m/incs[-1]/'UsbMassBoot.c'),'-o',str(out/'retry-test')]
r=subprocess.run(cmd,capture_output=True,text=True);print(r.stdout,r.stderr);r.check_returncode()
r=subprocess.run([str(out/'retry-test')],capture_output=True,text=True);(out/'retry-test.txt').write_text(r.stdout+r.stderr);print(r.stdout,r.stderr);r.check_returncode()
