#!/usr/bin/env python3
"""Regenerate checked-in SPIR-V with glslangValidator (host tool only)."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
for source,symbol,header in [('output_nv12.comp','outputNv12Spv','output_nv12_spv.h'),('output_yuv.frag','outputYuvSpv','output_yuv_spv.h'),('output_fullscreen.vert','outputFullscreenSpv','output_fullscreen_spv.h')]:
    subprocess.run(['glslangValidator','-V',str(root/'renderer/shaders'/source),'--vn',symbol,'-o',str(root/'renderer'/header)],check=True)
