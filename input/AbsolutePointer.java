import android.view.*;
import android.os.SystemClock;
import java.io.*;
import java.lang.reflect.*;
public class AbsolutePointer {
 public static void main(String[] args) throws Exception {
  Class<?> im=Class.forName("android.hardware.input.InputManager");
  Object manager=im.getMethod("getInstance").invoke(null);
  Method inject=im.getMethod("injectInputEvent",InputEvent.class,int.class);
  BufferedReader in=new BufferedReader(new InputStreamReader(System.in));
  int device=-1;for(int id:InputDevice.getDeviceIds()){InputDevice d=InputDevice.getDevice(id);if(d!=null&&d.getName().equals("EmuXR2 Mouse and Keyboard"))device=id;}
  float x=960,y=540; int buttons=0;long down=0;
  System.out.println("ready");System.out.flush();String line;
  while((line=in.readLine())!=null){try{
   String[] p=line.split(" "); int action=MotionEvent.ACTION_HOVER_MOVE;float wheel=0;
   if(p[0].equals("p")){x=Float.parseFloat(p[1]);y=Float.parseFloat(p[2]);action=buttons!=0?MotionEvent.ACTION_MOVE:MotionEvent.ACTION_HOVER_MOVE;}
   else if(p[0].equals("d")){buttons=1;down=SystemClock.uptimeMillis();action=MotionEvent.ACTION_DOWN;}
   else if(p[0].equals("u")){buttons=0;action=MotionEvent.ACTION_UP;}
   else if(p[0].equals("w")){wheel=Float.parseFloat(p[1]);action=MotionEvent.ACTION_SCROLL;}
   else continue;
   MotionEvent.PointerProperties property=new MotionEvent.PointerProperties();property.id=0;property.toolType=MotionEvent.TOOL_TYPE_MOUSE;
   MotionEvent.PointerCoords coords=new MotionEvent.PointerCoords();coords.x=x;coords.y=y;coords.pressure=buttons!=0?1:0;coords.setAxisValue(MotionEvent.AXIS_VSCROLL,wheel);
   MotionEvent event=MotionEvent.obtain(down,SystemClock.uptimeMillis(),action,1,new MotionEvent.PointerProperties[]{property},new MotionEvent.PointerCoords[]{coords},0,buttons,1,1,device,0,InputDevice.SOURCE_MOUSE,0);
   inject.invoke(manager,event,0);
   if(action==MotionEvent.ACTION_DOWN||action==MotionEvent.ACTION_UP){event.setAction(action==MotionEvent.ACTION_DOWN?MotionEvent.ACTION_BUTTON_PRESS:MotionEvent.ACTION_BUTTON_RELEASE);MotionEvent.class.getMethod("setActionButton",int.class).invoke(event,MotionEvent.BUTTON_PRIMARY);inject.invoke(manager,event,0);}
   event.recycle();
  }catch(Exception e){System.err.println(e);}}
 }
}
