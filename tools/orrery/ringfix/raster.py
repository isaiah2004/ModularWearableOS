"""Run existing raster regressions plus ringfix geometry/input oracles."""
import review as r

def main():
    for kind,cost in [('cost',True),('nocost',False)]:
        r.build(kind,cost=cost)
        obj=r.OUT/f'obj_{kind}'
        sim=r.OUT/f'sim_test_{kind}.o';test=r.OUT/f'raster_{kind}.o';exe=r.OUT/f'raster_{kind}.exe'
        r.command([r.GCC.with_name('objcopy.exe'),'--redefine-sym','main=sim_program_main',obj/'sim_nos_sim.c.o',sim],r.OUT/f'objcopy_{kind}.log')
        flags=['-O2','-std=c99','-Icore','-Inos','-Inos/fonts','-Isim']
        if cost:flags+=['-DGFX_COST_MODEL']
        r.command([r.GCC,*flags,'-c',r.ROOT/'tests/orrery_raster_test.c','-o',test],r.OUT/f'raster_build_{kind}.log')
        objects=[p for p in sorted(obj.glob('*.o')) if p.name not in ['sim_nos_sim.c.o','nos_faces_face_orrery.c.o']]
        r.command([r.GCC,*objects,sim,test,'-o',exe,'-lgdi32','-luser32','-lwinmm','-lm'],r.OUT/f'raster_link_{kind}.log')
        r.run(exe,r.ROOT/'tools/orrery/perf_round7.txt',f'raster_{kind}','--no-media')
        print('PASS raster',kind,flush=True)
if __name__=='__main__':main()
