# SPDX-License-Identifier: MIT
# Fetch only the official, pinned Quest kernel camera UAPI; no kernel checkout.
from pathlib import Path
import urllib.request,re,json
commit='6929f734ce0e602018790ff3a52dc7bad646af60'
base='https://raw.githubusercontent.com/facebookincubator/oculus-linux-kernel/'+commit+'/'
import sys
root=Path(sys.argv[1])
queue=[('media/'+n,'include/uapi/media/'+n) for n in ['msmb_camera.h','msmb_isp.h','msmb_ispif.h','msm_cam_sensor.h']]+[('linux/media.h','include/uapi/linux/media.h'),('linux/ion.h','drivers/staging/android/uapi/ion.h')]
seen=set()
while queue:
 local,remote=queue.pop(0)
 if local in seen:continue
 seen.add(local);data=urllib.request.urlopen(base+remote,timeout=30).read().decode()
 p=root/local;p.parent.mkdir(parents=True,exist_ok=True)
 p.write_text(data.replace('__user ','').replace(' __user','').replace('#define V4L2_PIX_FMT_SGRBG14','#undef V4L2_PIX_FMT_SGRBG14\n#define V4L2_PIX_FMT_SGRBG14'))
 for name in re.findall(r'#\s*include\s*[<"]([^>"]+)[>"]',data):
  if name.startswith('media/'):queue.append((name,'include/uapi/'+name))
  elif '/' not in name and name.endswith('.h') and local.startswith('media/'):queue.append(('media/'+name,'include/uapi/media/'+name))
(root/'source.json').write_text(json.dumps(dict(repository='facebookincubator/oculus-linux-kernel',commit=commit,headers=sorted(seen)),indent=2))
print('Fetched',len(seen),'headers')
