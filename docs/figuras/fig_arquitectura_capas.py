import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, Rectangle
plt.rcParams['font.family']='DejaVu Sans'
fig,ax=plt.subplots(figsize=(13,7.2),dpi=200); ax.set_xlim(0,130); ax.set_ylim(0,72); ax.axis('off')
HW='#1f3b63'; EM='#b45309'; GR='#6b7280'; GN='#1e7a46'
def box(x,y,w,h,fc='white',ec=HW,lw=1.4,ls='-',r=0.8,z=2):
    ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle=f"round,pad=0,rounding_size={r}",fc=fc,ec=ec,lw=lw,ls=ls,zorder=z))
def T(x,y,s,fs=8,c='#111',w='normal',ha='center',va='center',st='normal'):
    ax.text(x,y,s,fontsize=fs,color=c,fontweight=w,ha=ha,va=va,style=st,zorder=6)
def arrow(x1,y1,x2,y2,c=HW,lw=1.4,both=False,ls='-'):
    ax.annotate('',xy=(x2,y2),xytext=(x1,y1),arrowprops=dict(arrowstyle='<|-|>' if both else '-|>',color=c,lw=lw,ls=ls,mutation_scale=9),zorder=5)
# layer backgrounds
box(1,40,128,31,fc='#fff9f1',ec=EM,lw=1,ls=(0,(4,3)))
T(3,69,'CAPA DE EMULACIÓN  (firmware en cada STM32F303K8: planta + convertidor promediado + control local, ISR de 10 kHz)',7.6,EM,'bold',ha='left')
box(1,1,128,37,fc='#f4f7fb',ec=HW,lw=1,ls=(0,(4,3)))
T(3,36.2,'CAPA DE SUPERVISIÓN Y ADQUISICIÓN  (hardware real)',7.6,HW,'bold',ha='left')
# DC bus rail
ax.plot([5,125],[45.4,45.4],color='#222',lw=5,zorder=3,solid_capstyle='round')
T(65,42.6,'BUS DC 48 V ±5 %  (45.6–50.4 V) — emulado, sin etapa de potencia física',7.4,'#222','bold')
subs=[('PV','Modelo 1-diodo\nG(h), T celda','Boost · MPPT P&O',  '0x10/0x20','#e8a317','→'),
      ('Eólica','Thevenin V_oc(f), R_eq(f)\nrectificador pasivo','Boost · MPPT P&O','0x11/0x21','#2f6db5','→'),
      ('Batería','Thevenin 1.er orden\nSOC ×60, límites 0.5C','Bidireccional · regula V_bus','0x12/0x22','#7a4bb5','↔'),
      ('Carga','Perfiles res./ind.\nη(FP), I_dc = P/(η V_bus)','Sumidero con C_in','0x13/0x23','#c0392b','←')]
xs=[8,38,68,98]
for (n,m,c,a,col,d),x in zip(subs,xs):
    box(x,52,25,14,fc='white',ec=col,lw=1.8)
    ax.add_patch(Rectangle((x,52),25,2.8,fc=col,ec='none',zorder=3))
    T(x+12.5,53.4,n,8.6,'white','bold')
    T(x+12.5,60.6,m,6.5); T(x+12.5,56.3,c,6.3,col,'bold')
    # converter block to bus
    cx=x+12.5
    box(cx-5,48.6,10,2.8,fc='#fff7ed',ec=EM,lw=1,r=0.4,z=4); T(cx,50,'DC/DC prom.',5.2,EM)
    ax.plot([cx,cx],[51.4,52],color='#222',lw=1.2,zorder=3)
    (arrow(cx,48.6,cx,45.9,c="#222",lw=1.1,both=(d=="↔")) if d!="←" else arrow(cx,45.9,cx,48.6,c="#222",lw=1.1))
    ax.plot([cx+6.5,cx+6.5],[34.5,52],color=GR,lw=1.1,ls=(0,(2,2)),zorder=4)
    # I2C drop from STM to ESP (hardware)
    T(cx,67.4 if False else 38.9,'',1)
# I2C bus line in hardware layer
ax.plot([8,123],[27,27],color='#222',lw=3,zorder=3); ax.plot([8,123],[25.4,25.4],color='#222',lw=1.4,ls=(0,(3,2)),zorder=3)
T(125,27.2,'I²C',6.8,'#222','bold',ha='right'); T(125,24.2,'100 kHz',5.8,GR,ha='right')
for (n,m,c,a,col,d),x in zip(subs,xs):
    cx=x+12.5
    # vertical tie crossing bus layer boundary: dashed gray (STM pins physical)
    ax.plot([cx,cx],[27,48.6],color=GR,lw=0,zorder=1)
    box(cx-8,29.5,16,5,fc='white',ec=col,lw=1.6)
    T(cx,32,'STM32 · '+a,6.2,'#111','bold')
    ax.plot([cx,cx],[27,29.5],color='#222',lw=1.3,zorder=3)
    arrow(cx,52,cx,35.2,c=GR,lw=0.9,ls=(0,(2,2))) if False else None
# ESP32
box(38,6,26,15,fc='#eaf5ee',ec=GN,lw=1.8)
T(51,18.6,'ESP32-S3',9.5,GN,'bold'); T(51,16.2,'maestro I²C · gateway MQTT',6.2,GR)
T(51,12.8,'SCADA 1 Hz · estados S0–S9',6.6); T(51,10.2,'recorte ΔP · deslastre 60/40/25 %',6.2); T(51,7.8,'arranque en negro (4 × 6 s)',6.2)
ax.plot([51,51],[21,27],color='#222',lw=1.6,zorder=3); arrow(51,21,51,26.6,both=True,c='#222',lw=1.4)
T(53,23.9,'sondeo/comandos',5.6,GR,ha='left')
T(20.5+9+1,43.2,'misma placa:\nfirmware',5.4,GR,ha='left') if False else None
T(31,39,'cada STM32 aloja su modelo (misma placa)',5.8,GR,ha='left',st='italic')
# SCADA -> emulation (limits) dashed up arrows
for x in [20.5,50.5,80.5,110.5]:
    pass
# Wi-Fi/MQTT -> PC
box(78,6,48,15,fc='#f3f4f6',ec=GR,lw=1.4)
T(102,18.6,'PC',9,'#111','bold')
for k,(a,b) in enumerate([('Mosquitto','broker'),('Logger','CSV crudo'),('Tablero','en vivo'),('Análisis','métricas')]):
    x=80+k*11.5; box(x,7.2,10.5,8,fc='white',ec=GR,lw=1)
    T(x+5.25,12.8,a,6.4,'#111','bold'); T(x+5.25,9.4,b,5.6,GR)
arrow(64,13.5,78,13.5,c=GN,lw=2.2,both=True)
T(71,16,'Wi-Fi · MQTT',6.2,GN,'bold'); T(71,10.8,'microred/<s>',5.2,GR); T(71,9.2,'microred_ctrl/…',5.2,GR)
# left: clock / legend text
box(5,6,29,15,fc='white',ec=GR,lw=1,ls=(0,(3,2)))
T(19.5,18.4,'Reloj de simulación común',6.8,'#111','bold')
T(19.5,14.4,'1 día = 360 s (1 h = 15 s)\ninicia a las 6 h simuladas',6.2)
T(19.5,9.3,'SOC acelerado 60×',6.2)
# vertical arrows ESP32 command influence to emulation: single bracket
ax.annotate('',xy=(65,47.2),xytext=(65,47.2))
# legend
ax.add_patch(Rectangle((2,-2.6),3,1.5,fc='white',ec=HW,lw=1.4,clip_on=False)); T(6.5,-1.85,'hardware real',6.6,ha='left')
ax.add_patch(Rectangle((24,-2.6),3,1.5,fc='#fff7ed',ec=EM,lw=1.4,ls=(0,(3,2)),clip_on=False)); T(28.5,-1.85,'emulado en firmware',6.6,ha='left')
T(128,-1.85,'Flechas del bus: sentido de la potencia (PV, eólica → bus → carga; batería bidireccional)',6.2,GR,ha='right')
# power-flow arrows on bus
arrow(20.5,52,20.5,47.9,c='#222',lw=0) if False else None
for x,dr in [(20.5,'down'),(50.5,'down'),(80.5,'both'),(110.5,'up')]:
    pass
ax.set_ylim(-4,72)
fig.savefig('arquitectura_capas.png',bbox_inches='tight',pad_inches=0.1,facecolor='white')
