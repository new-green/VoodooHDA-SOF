// rec <device name|list> <seconds> [out.raw]: AudioQueue capture (48k/2ch/S16) from a named input device, prints stats
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static FILE *fo; static double sum[2]; static long n; static int peak[2]; static long zeros;
static void cb(void *u, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp *t, UInt32 np, const AudioStreamPacketDescription *d){
  short *s=b->mAudioData; UInt32 fr=b->mAudioDataByteSize/4;
  for(UInt32 i=0;i<fr;i++) for(int c=0;c<2;c++){int v=s[2*i+c]; sum[c]+=(double)v*v; if(abs(v)>peak[c])peak[c]=abs(v); if(!v)zeros++;}
  n+=fr; if(fo) fwrite(s,4,fr,fo); AudioQueueEnqueueBuffer(q,b,0,NULL);
}
int main(int argc,char**argv){
  if(argc<2){fprintf(stderr,"usage: rec <name|list> <sec> [out.raw]\n");return 2;}
  AudioObjectPropertyAddress a={kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
  UInt32 sz; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&a,0,NULL,&sz);
  AudioDeviceID ids[64]; AudioObjectGetPropertyData(kAudioObjectSystemObject,&a,0,NULL,&sz,ids);
  CFStringRef uid=NULL;
  for(UInt32 i=0;i<sz/sizeof(AudioDeviceID);i++){
    CFStringRef nm; UInt32 s2=sizeof(nm); AudioObjectPropertyAddress p={kAudioObjectPropertyName,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(ids[i],&p,0,NULL,&s2,&nm); char buf[256]; CFStringGetCString(nm,buf,256,kCFStringEncodingUTF8);
    CFStringRef u; p.mSelector=kAudioDevicePropertyDeviceUID; s2=sizeof(u); AudioObjectGetPropertyData(ids[i],&p,0,NULL,&s2,&u);
    char ub[256]; CFStringGetCString(u,ub,256,kCFStringEncodingUTF8);
    if(!strcmp(argv[1],"list")) printf("%u '%s' uid=%s\n",ids[i],buf,ub);
    else if(!strcmp(buf,argv[1])) uid=u;
  }
  if(!strcmp(argv[1],"list")) return 0;
  if(!uid){fprintf(stderr,"device not found\n");return 1;}
  AudioStreamBasicDescription f={48000,kAudioFormatLinearPCM,kLinearPCMFormatFlagIsSignedInteger|kLinearPCMFormatFlagIsPacked,4,1,4,2,16,0};
  AudioQueueRef q; OSStatus e=AudioQueueNewInput(&f,cb,NULL,NULL,NULL,0,&q); if(e){printf("new %d\n",e);return 1;}
  e=AudioQueueSetProperty(q,kAudioQueueProperty_CurrentDevice,&uid,sizeof(uid)); if(e){printf("dev %d\n",e);return 1;}
  for(int i=0;i<4;i++){AudioQueueBufferRef b; AudioQueueAllocateBuffer(q,4800*4,&b); AudioQueueEnqueueBuffer(q,b,0,NULL);}
  fo=argc>3?fopen(argv[3],"wb"):NULL;
  e=AudioQueueStart(q,NULL); if(e){printf("start %d\n",e);return 1;}
  CFRunLoopRunInMode(kCFRunLoopDefaultMode,atof(argv[2]),false);
  AudioQueueStop(q,true); if(fo)fclose(fo);
  printf("%-36s %.2f s  rms L %6.1f R %6.1f dBFS  peak L %5d R %5d  zeros %ld\n",argv[1],n/48000.0,
    10*log10(sum[0]/n/1073741824.0+1e-20),10*log10(sum[1]/n/1073741824.0+1e-20),peak[0],peak[1],zeros);
  return 0;
}
