import pandas as pd, json, numpy as np, matplotlib
matplotlib.use('Agg'); import matplotlib.pyplot as plt
df=pd.read_csv('../../datos/P0_demo.csv')
def sub(t):
    d=df[df.topic==t]; j=pd.json_normalize(d.payload.map(json.loads)); j.insert(0,'t',d.t_rel_s.values); return j
pv=sub('microred/pv'); k=sub('microred_ctrl/estado'); ev=sub('microred_ctrl/sistema').dropna(subset=['evento'])
wr=k.t[k.hora.diff()<-10].values
INK='#222222'; MUT='#666666'; GR='#2E7D4F'; BL='#2F6DB5'; RD='#D9485F'; OR='#C97B00'
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':11,'axes.edgecolor':'#999','axes.spines.top':False,'axes.spines.right':False})
fig,ax=plt.subplots(3,1,figsize=(11,8.2),sharex=True,gridspec_kw={'height_ratios':[1.1,1.2,0.7]})
for a in ax:
    a.grid(axis='y',color='#e6e6e6',lw=0.8); a.set_axisbelow(True)
    for w in wr: a.axvline(w,color='#bbb',ls=':',lw=1.2)
ax[0].plot(pv.t,pv.p,color=GR,lw=1.8); ax[0].set_ylabel('Potencia PV [W]')
ax[0].set_title('Prueba P0: 3 amaneceres consecutivos con la PV corregida',loc='left',fontweight='bold',color=INK)
for i,w in enumerate(wr): ax[0].annotate('amanecer %d\n(arranca solo)'%(i+1),(w,6),(w-8,45),fontsize=9.5,color=MUT,ha='right',arrowprops=dict(arrowstyle='-',color='#999'))
ax[1].plot(k.t,k.soc,color=BL,lw=1.8); ax[1].set_ylabel('SOC batería [%]')
ax[1].axhline(20,color=RD,ls='--',lw=1.3); ax[1].text(1190,20.8,'meta: no bajar de 20 %',color=RD,fontsize=10,ha='right')
for _,r in ev.iterrows():
    e=k.iloc[(k.t-r.t).abs().argmin()]
    s6='DESLASTRE' in r.evento; ax[1].plot(r.t,e.soc,'v' if s6 else '^',color=OR if s6 else GR,ms=9,mec='white',mew=1.5)
m=k.soc.idxmin(); ax[1].annotate('mínimo 24,5 %',(k.t[m],k.soc[m]),(k.t[m]-150,46),fontsize=10,color=INK,arrowprops=dict(arrowstyle='-',color='#999'))
ax[1].plot([],[],'v',color=OR,label='deslastre (S6)'); ax[1].plot([],[],'^',color=GR,label='restauración (S7)'); ax[1].legend(loc='upper right',frameon=False,fontsize=10)
ax[2].plot(k.t,k.vbus,color=INK,lw=1.8); ax[2].axhspan(45.6,50.4,color='#DDEBE2',alpha=.9,lw=0); ax[2].set_ylim(44,52); ax[2].set_ylabel('Vbus [V]')
ax[2].text(8,50.9,'banda 48 V ±5 %',color=GR,fontsize=10); ax[2].set_xlabel('Tiempo [s]  (día simulado = 360 s)')
ax[2].set_xlim(0,1200)
plt.tight_layout(); plt.savefig('P0_resultados.png',dpi=170)
