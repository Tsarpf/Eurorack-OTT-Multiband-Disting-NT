import sys,json,csv
from pathlib import Path
import numpy as np,soundfile as sf
from scipy.signal import welch
from scipy.ndimage import gaussian_filter1d
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import argparse
parser=argparse.ArgumentParser(description='Plot a completed low-formant comparison')
parser.add_argument('--root', type=Path, required=True)
root=parser.parse_args().root.resolve();run=Path(json.load(open(root/'run.json'))['directory']);report=json.load(open(root/'comparison.json'));out=root/'analysis';out.mkdir(exist_ok=True)
rows=[]
for case in report['cases']:
 p=case['parameters']
 for source in ['live','native']:
  for m in case.get(source,[]):
   rows.append(dict(source=source,formant=p['Formant Shift'],width=round(p['Filter Width']*100),depth=round(p['Envelope Depth']*100),**{k:v for k,v in m.items() if k!='power_fraction_above'},**{f'power_above_{k}':v for k,v in m['power_fraction_above'].items()}))
with (out/'metrics.csv').open('w') as f:
 writer=csv.DictWriter(f,fieldnames=rows[0].keys());writer.writeheader();writer.writerows(rows)
fig,axes=plt.subplots(1,3,figsize=(14,4.8),sharey=True);colors={0:'#777777',-24:'#dd7c19',-30:'#8d59bc',-36:'#197da3'}
for ax,depth in zip(axes,[100,150,200]):
 for formant in [0,-24,-36]:
  case=next(c for c in report['cases'] if c['parameters']['Formant Shift']==formant and c['parameters']['Filter Width']==.75 and c['parameters']['Envelope Depth']==depth/100)
  for source,style in [('live','-'),('native','--')]:
   if source not in case:continue
   path=run/'cases'/case['id']/'output.wav' if source=='live' else root/'native'/(case['id']+'.wav')
   x,sr=sf.read(path);offset=case[source+'_offset'];x=x[int((.5+offset+.8)*sr):int((2.5+offset-.15)*sr),0]
   f,p=welch(x,sr,nperseg=16384);freq=np.geomspace(40,20000,1500);db=gaussian_filter1d(np.interp(np.log(freq),np.log(np.maximum(f,1)),10*np.log10(np.maximum(p,1e-30))),3);db-=max(db)
   ax.plot(freq,db,color=colors[formant],ls=style,lw=1.4,label=f'{source.title()} {formant:+d} st')
 ax.set(xscale='log',xlim=(40,20000),ylim=(-90,3),title=f'Depth {depth}%',xlabel='Frequency (Hz)');ax.grid(alpha=.2)
axes[0].set_ylabel('Spectrum relative to its peak (dB)');axes[2].legend(fontsize=8,loc='lower left')
fig.suptitle('Low Formant: Ableton vs current native vocoder\nWhite noise · Width75% · 40 bands · Enhance On · Precise · Release30 ms',fontsize=12)
fig.tight_layout();fig.savefig(out/'spectra.png',dpi=160)
# Every setting's loud saw level, relative to its own zero-formant baseline.
fig,axes=plt.subplots(1,3,figsize=(12,4.5),sharey=True)
for ax,width in zip(axes,[50,75,100]):
 for depth,color in zip([100,150,200],['#3181bb','#cd7722','#b4488e']):
  for source,style in [('live','-'),('native','--')]:
   points=[next((r for r in rows if r['source']==source and r['name']=='saw_loud' and r['width']==width and r['depth']==depth and r['formant']==shift),None) for shift in [0,-24,-30,-36]]
   if any(r is None for r in points):continue
   y=[r['rms_dbfs']-points[0]['rms_dbfs'] for r in points];ax.plot([0,-24,-30,-36],y,style,marker='o',color=color,label=f'{source.title()} Depth{depth}')
 ax.set(title=f'Width {width}%',xlabel='Formant (semitones)',xlim=(-37,1),xticks=[-36,-30,-24,0]);ax.grid(alpha=.2)
axes[0].set_ylabel('Level change from Formant0 (dB)');axes[2].legend(fontsize=7)
fig.suptitle('110 Hz saw at −6 dBFS peak · self-carrier · Enhance On');fig.tight_layout();fig.savefig(out/'saw-levels.png',dpi=160)
print(out)
