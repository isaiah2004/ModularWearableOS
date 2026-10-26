#include "apps/native_tile.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
/* Independent floating reference: integrate authored path poses, interpolating
 * both the density knots and source samples in physical pixel coordinates. */
static double reference(const uint8_t *p,int h,int y,int length,int d) {
 if(length>90)length=90;
 int tail=length>0?length/2:0;
 if(!tail)return y>=0&&y<h?p[y]:0;
 double knot[]={-tail,-.75*tail,-.5*tail,-.25*tail,0,1,2};
 const double ratio[]={10,20,40,50,100,40,20};
 double sum=0,area=0;
 for(int step=0;step<=4*(tail+2);step++) {
  double pose=-tail+step*.25;int segment=0;
  while(segment<5&&pose>knot[segment+1])segment++;
  double density=ratio[segment]+(ratio[segment+1]-ratio[segment])*(pose-knot[segment])/(knot[segment+1]-knot[segment]);
  double at=y-d*pose;int lo=(int)floor(at);double f=at-lo;
  double sample=(lo>=0&&lo<h?p[lo]:0)*(1-f)+(lo+1>=0&&lo+1<h?p[lo+1]:0)*f;
  double trapezoid=(step==0||step==4*(tail+2))?.125:.25;
  area+=density*trapezoid;sum+=density*trapezoid*sample;
 }
 assert(fabs(area-(41.25*tail+100))<1e-8);
 return sum/area;
}
int main(void) {
 uint8_t p[201],impulse[201]={0};impulse[100]=255;
 unsigned checked=0;
 for(int length=-1;length<=120;length++) {
  if(length>90&&length!=120)continue;
  native_trail_kernel k;native_trail_kernel_make(&k,length);
  uint64_t sum=0;for(unsigned i=0;i<sizeof k.weight/sizeof k.weight[0];i++)sum+=k.weight[i];
  assert(sum==k.total&&sum>0&&sum*255+sum/2<=UINT32_MAX);
  int tail=length>90?45:length>0?length/2:0;
  assert(k.tail==tail);
  assert(k.total==(tail?(uint32_t)(1320*tail*tail+3200*tail):1));
  for(int pattern=0;pattern<3;pattern++) {
   for(int i=0;i<201;i++)p[i]=pattern==0?impulse[i]:pattern==1?173:(uint8_t)((i*73+i*i*19)%256);
   for(int d=-1;d<=1;d+=2)for(int y=-48;y<249;y++) {
    double expected=reference(p,201,y,length,d);
    unsigned actual=native_trail_sample(&k,p,1,201,0,y,d);
    assert(fabs(actual-expected)<=.50000001);
    assert(actual==native_trail_pixel(p,1,201,0,y,length,d));checked++;
    if(pattern==1&&y>=48&&y<153)assert(actual==173);
   }
  }
  for(int y=-48;y<249;y++)assert(native_trail_pixel(impulse,1,201,0,y,length,1)==native_trail_pixel(impulse,1,201,0,200-y,length,-1));
  assert(native_trail_sample(&k,NULL,1,201,0,100,1)==0);
  assert(native_trail_sample(&k,p,1,201,-1,100,1)==0);
  assert(native_trail_sample(&k,p,1,201,1,100,1)==0);
  assert(native_trail_sample(&k,p,1,0,0,100,1)==0);
 }
 const int levels[]={0,15,30,60,90};
 for(int i=0;i<5;i++)assert(native_smear_exposure(levels[i]*5.0f/M_FPS)==levels[i]*256);
 assert(native_smear_exposure(100)==90*256);
 for(int e=0;e<=90*256;e++){int lo,hi;unsigned mix;native_prepared_levels(e,&lo,&hi,&mix);assert(lo>=0&&hi<=4&&mix<=256);}
 /* The required accelerate/brake sequence must stay stateless and normalized;
  * revisiting a speed reconstructs the identical continuous cache mixture. */
 const float steps[]={10,20,40,50,100,40,20};int exposure[7];
 for(unsigned i=0;i<7;i++) {
  float width=fminf(90.0f,steps[i]*M_FPS*.2f);
  exposure[i]=native_smear_exposure(steps[i]);assert(exposure[i]==(int)lroundf(width*256));
  int lo,hi;unsigned mix;native_prepared_levels(exposure[i],&lo,&hi,&mix);
  const int widths[]={0,15*256,30*256,60*256,90*256};
  assert(lo>=0 && hi<=4 && widths[lo]<=exposure[i] && widths[hi]>=exposure[i]);
  assert(abs((widths[lo]*(int)(256-mix)+widths[hi]*(int)mix)/256-exposure[i])<=90);
 }
 assert(exposure[1]==exposure[6] && exposure[2]==exposure[5]);
 printf("PASS normalized continuous smear: %u independent pixel comparisons, area, bounds, clamping, overflow, reflection, exposure ladder\n",checked);
}
