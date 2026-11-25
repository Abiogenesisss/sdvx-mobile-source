"""Export the original result reward cues; no package build."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
from bemani.format import TwoDX

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('data',type=Path)
p.add_argument('assets',type=Path)
p.add_argument('ffmpeg',type=Path)
p.add_argument('work',type=Path)
a=p.parse_args()
bank=TwoDX((a.data/'sound/sys_sd.2dx').read_bytes())
out=a.assets/'result_force/audio'
out.mkdir(parents=True,exist_ok=True)
for index,name in [(183,'class'),(184,'plus'),(185,'level'),(55,'total_build'),(59,'total_info')]:
    subprocess.run([str(a.ffmpeg),'-v','error','-y','-i','pipe:0','-ac','2','-ar','44100',
                    '-c:a','pcm_s16le',str(out/f'{name}.wav')],input=bank.read_file(bank.filenames[index]),check=True)
for crew,number in [('rasis','vo032'),('grace','vo033')]:
    decoded=a.work/crew
    if not (decoded/'result12.wav').exists():
        subprocess.run([sys.executable,str(Path(__file__).with_name('extract-audio-bank.py')),
                        str(a.data/f'sound/voice/{number}_{crew}_nabra/{number}_10_result.s3p'),
                        str(a.ffmpeg),str(decoded)],check=True)
    for n in range(12,15):
        shutil.copyfile(decoded/f'result{n}.wav',a.assets/f'crew/{crew}/result{n}.wav')
print('Prepared original FORCE voices and result effects')
