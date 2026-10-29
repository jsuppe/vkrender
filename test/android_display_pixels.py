#!/usr/bin/env python3
"""Validate two real Android outputs via compositor captures, not optical scanout.
Requires foreground *fresh/unpaused* renderer with displayOutputs=secondary ID.
Does not install/reset apps. Pauses scene time, changes secondary crop, restores
full crop and resumes scene time in finally. Requires adb, Pillow and numpy.
"""
import argparse, subprocess, time
from pathlib import Path
from PIL import Image
import numpy as np
p=argparse.ArgumentParser()
p.add_argument('--serial',required=True)
p.add_argument('--primary-physical',required=True)
p.add_argument('--secondary-physical',required=True)
p.add_argument('--secondary-id',required=True,type=int)
p.add_argument('--output',required=True,type=Path)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
adb=['adb','-s',a.serial]
def shell(*args):subprocess.run(adb+['shell',*map(str,args)],check=True,stdout=subprocess.DEVNULL)
def config(crop=None):
 args=['am','start','-n','dev.vkrender.renderer/.MainActivity','-f','0x20000000','--es','displayOutputs',a.secondary_id]
 if crop:args+=['--es','displayCrops',str(a.secondary_id)+'='+crop]
 shell(*args);time.sleep(1)
def capture(name,id):
 with (a.output/(name+'.png')).open('wb')as f:subprocess.run(adb+['exec-out','screencap','-p','-d',id],stdout=f,check=True)
 return np.array(Image.open(a.output/(name+'.png')).convert('RGB'))
shell('input','keyevent','62')
try:
 config()
 primary=capture('primary',a.primary_physical);secondary=capture('secondary-full',a.secondary_physical)
 h,w,_=primary.shape
 expected=np.array(Image.fromarray(primary).resize((secondary.shape[1],secondary.shape[0]),Image.Resampling.NEAREST))
 # System bars/rounded corners are overlays, not pixels in the Vulkan image.
 interior=(slice(h//10,h*8//10),slice(w//10,w*9//10))
 difference=np.abs(expected.astype(int)-secondary.astype(int))[interior]
 full_max=int(difference.max());assert full_max==0,('full interior mismatch',full_max)
 config('0.25,0.25,0.5,0.5')
 secondary=capture('secondary-crop',a.secondary_physical)
 expected=np.array(Image.fromarray(primary).crop((w//4,h//4,w*3//4,h*3//4)).resize((secondary.shape[1],secondary.shape[0]),Image.Resampling.NEAREST))
 difference=np.abs(expected.astype(int)-secondary.astype(int));crop_max=int(difference.max())
 assert crop_max==0,('crop mismatch',crop_max,float(difference.mean()))
 result=f'PASS full-frame interior max_byte_error={full_max}; center-half crop all_pixels max_byte_error={crop_max}\n'
 (a.output/'results.txt').write_text(result);print(result,end='')
finally:
 config();shell('input','keyevent','62')
