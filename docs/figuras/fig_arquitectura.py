import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, Rectangle
plt.rcParams['font.family']='DejaVu Sans'
fig,ax=plt.subplots(figsize=(13,6.4),dpi=200); ax.set_xlim(0,130); ax.set_ylim(0,64); ax.axis('off')
HW='#1f3b63'; EM='#b45309'; GR='#6b7280'
def box(x,y,w,h,fc='white',ec=HW,lw=1.4,ls='-',r=0.8,z=2):
    p=FancyBboxPatch((x,y),w,h,boxstyle=f"round,pad=0,rounding_size={r}",fc=fc,ec=ec,lw=lw,ls=ls,zorder=z); ax.add_patch(p)
def T(x,y,s,fs=8,c='#111',w='normal',ha='center',va='center',st='normal'):
    ax.text(x,y,s,fontsize=fs,color=c,fontweight=w,ha=ha,va=va,style=st,zorder=6)
def arrow(x1,y1,x2,y2,c=HW,lw=1.4,both=False,ls='-'):
    ax.annotate('',xy=(x2,y2),xytext=(x1,y1),arrowprops=dict(arrowstyle='<|-|>' if both else '-|>',color=c,lw=lw,ls=ls,mutation_scale=9),zorder=5)
# --- STM32 column
box(1,2,60,60,fc='#f4f7fb',ec=GR,lw=1,ls=(0,(4,3)))
T(31,59.3,'4 × STM32F303K8 (hardware real)',9.5,HW,'bold')
subs=[('PV','Modelo 1-diodo, G(h), T celda','Boost + MPPT P&O','0x10 / 0x20','#e8a317'),
      ('Eólica','Thevenin V_oc(f), R_eq(f)','Boost + MPPT P&O','0x11 / 0x21','#2f6db5'),
      ('Batería','Thevenin 1.er orden, SOC×60','Bidireccional: regula V_bus','0x12 / 0x22','#7a4bb5'),
      ('Carga','Perfiles res./ind., η(FP)','Sumidero I_dc = P/(η V_bus)','0x13 / 0x23','#c0392b')]
for i,(n,m,c,a,col) in enumerate(subs):
    y=44-i*14.2
    box(3,y,56,12.4,fc='white',ec=col,lw=1.8)
    ax.add_patch(Rectangle((3,y),2.2,12.4,fc=col,ec='none',zorder=3))
    T(8,y+9.9,n,9.5,col,'bold',ha='left')
    box(7,y+1.9,37,6.6,fc='#fff7ed',ec=EM,lw=1,ls=(0,(3,2)),r=0.5,z=2)
    T(8,y+6.3,m,7.2,ha='left'); T(8,y+3.2,c,7.2,ha='left')
    T(43,y+9.9,'lazo de control local',5.6,HW,ha='right',st='italic')
    T(57.5,y+9.9,a,7,GR,ha='right')
    T(57.5,y+0.7,'una sola ISR a 10 kHz',5.6,GR,ha='right',st='italic')
# --- I2C bus
bx=67
ax.plot([bx,bx],[5,57],color='#222',lw=3.2,zorder=3)
ax.plot([bx+2.6,bx+2.6],[5,57],color='#222',lw=1.6,ls=(0,(3,2)),zorder=3)
T(bx+1.3,60.2,'Bus I²C',8.5,'#222','bold'); T(bx+1.3,58.2,'100 kHz · SDA/SCL',6.5,GR)
for i in range(4):
    y=44-i*14.2+6.2
    ax.plot([59,bx],[y,y],color='#222',lw=1.2,zorder=3); ax.plot([bx,bx+2.6],[y,y],color='#222',lw=0.8,zorder=3)
# --- ESP32
box(75,14,25,36,fc='#eaf5ee',ec='#1e7a46',lw=1.8)
T(87.5,47,'ESP32-S3',10,'#1e7a46','bold'); T(87.5,44.6,'maestro I²C · gateway · SCADA',6.8,GR)
for j,s in enumerate(['Sondeo 1 Hz de las 4 placas','P_neta del resto → batería','Máquina de estados S0–S9:','NORMAL · EXCEDENTE · DEFICIT','SEGURO · ARRANQUE (4 × 6 s)','Recorte ΔP · deslastre 60/40/25 %']):
    T(77,40-j*3.9,s,6.6,ha='left')
ax.plot([bx+2.6,75],[31,31],color='#222',lw=1.4,zorder=3); arrow(bx+2.6,31,75,31,both=True,c='#222')
# --- bus DC emulado
box(75,2.5,25,7.8,fc='#fff7ed',ec=EM,lw=1.6,ls=(0,(4,3)))
T(87.5,8,'Bus DC 48 V emulado',7.6,EM,'bold'); T(87.5,5,'banda ±5 % (45.6–50.4 V) · software',6.4,EM)
T(87.5,0.9,'sin etapa de potencia física',6,EM,st='italic')
arrow(87.5,14,87.5,10.4,c=EM,both=True,ls=(0,(3,2)))
# --- PC
box(107,10,22,46,fc='#f3f4f6',ec=GR,lw=1.4)
T(118,53,'PC',10,'#111','bold')
for k,(a,b) in enumerate([('Mosquitto','broker MQTT'),('Logger','CSV crudo con marca de tiempo'),('Tablero en vivo','Python · estados y alarmas'),('Análisis','métricas reproducibles')]):
    y=42-k*10.2; box(109,y,18,8.6,fc='white',ec=GR,lw=1)
    T(118,y+6,a,7.6,'#111','bold'); T(118,y+2.7,b,5.6,GR)
arrow(100,38,107,38,c='#1e7a46',lw=2.2)
T(103.5,41.2,'Wi-Fi',6.5,'#1e7a46','bold'); T(103.5,34.6,'MQTT',6.5,'#1e7a46','bold')
arrow(107,22,100,22,c='#1e7a46',lw=1.4); T(103.5,24.5,'cmd',6,'#1e7a46')
T(103.5,19.5,'auto, load,\nblackstart',5.2,GR)
T(118,6,'microred/<s>, _conv, _extra\nmicrored_ctrl/estado (1 Hz), /sistema',5.2,GR)
# legend
ax.add_patch(Rectangle((1,-3.4),3,1.6,fc='white',ec=HW,lw=1.4,clip_on=False)); T(5.5,-2.6,'hardware real',6.6,ha='left')
ax.add_patch(Rectangle((22,-3.4),3,1.6,fc='#fff7ed',ec=EM,lw=1.4,ls=(0,(3,2)),clip_on=False)); T(26.5,-2.6,'emulado en firmware',6.6,ha='left')
T(129,-2.6,'Reloj de simulación común: 1 día = 360 s (1 h = 15 s); SOC acelerado 60×',6.6,GR,ha='right')
ax.set_ylim(-5,64)
fig.savefig('arquitectura.png',bbox_inches='tight',pad_inches=0.1,facecolor='white')
