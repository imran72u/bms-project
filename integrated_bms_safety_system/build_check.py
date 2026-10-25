from pathlib import Path
root=Path(__file__).parent
required=['IntegratedBMS.ino','diagram.json','src/bms_engine.c','src/relay.c','src/anomaly.c','src/FaultStateMachine.cpp','src/TelemetryEngine.h','src/WiFiReconnectSM.h','src/FlickerFreeLCD.h']
missing=[x for x in required if not (root/x).exists()]
print('Required files:', 'OK' if not missing else 'MISSING '+', '.join(missing))
allsrc='\n'.join(p.read_text(errors='ignore') for p in (root/'src').glob('*') if p.is_file())
print('delay() calls:', allsrc.count('delay('))
print('Project status:', 'READY FOR HARDWARE/WOKWI BUILD' if not missing and allsrc.count('delay(')==0 else 'REVIEW NEEDED')
