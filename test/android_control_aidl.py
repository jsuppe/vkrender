#!/usr/bin/env python3
"""Ensure appended Binder control calls preserve the deployed transaction prefix."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
expected=['acquireInputSurface','releaseInputSurface','acquireInputSurfaceSlot','releaseInputSurfaceSlot',
          'acquireInputSurfaceWithOwner','releaseInputSurfaceWithOwner','getControlVersion',
          'getControlCapabilities','getControlStatus','submitControl','getPreviewJpeg','getDisplayOutputsStatus','configureDisplayOutput']
files=list((root/'android').rglob('IVkRendererService.aidl'))
assert len(files)==5,files
canonical=None
for path in files:
    source=re.sub(r'/\*.*?\*/','',path.read_text(),flags=re.S)
    declarations=re.findall(r'\b(?:Surface|void|int|String|byte\[\])\s+(\w+)\s*\(([^)]*)\)\s*;',source)
    assert [name for name,_ in declarations]==expected,path
    if canonical is None:canonical=declarations
    assert declarations==canonical,path
print('5 Binder interface copies preserve all legacy transaction IDs and match')
