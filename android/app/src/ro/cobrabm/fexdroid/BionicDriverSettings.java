package ro.cobrabm.fexdroid;

import android.app.*;
import android.content.*;
import android.net.Uri;
import android.os.Build;
import android.widget.*;
import java.io.*;
import java.nio.*;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.zip.*;
import org.json.JSONObject;

/** Immutable imported HALs stay outside the replaceable bundled runtime. Changes apply next start. */
final class BionicDriverSettings {
 static final int IMPORT_DRIVER=30;
 private static final int MAX_LIBRARY=64*1024*1024;
 private static final java.util.concurrent.atomic.AtomicBoolean importing=new java.util.concurrent.atomic.AtomicBoolean();
 static String selection(Context c){
  SharedPreferences p=c.getSharedPreferences("iss",0);
  // Older builds silently selected Gen8 for every phone, including Adreno 7xx.
  if(p.getInt("driver_schema",0)<2){
   String previous=p.getString("bionic_driver","auto");
   p.edit().putString("bionic_driver","gen8".equals(previous)?"auto":previous).putInt("driver_schema",2).commit();
  }
  return p.getString("bionic_driver","auto");
 }
 static File customLibrary(Context c){
  String hash=c.getSharedPreferences("iss",0).getString("custom_driver_hash","");
  if(!hash.matches("[0-9a-f]{64}"))hash="missing";
  return new File(c.getFilesDir(),"issport/custom-drivers/"+hash+"/libvulkan_freedreno.so");
 }
 static void add(Activity a,LinearLayout parent){
  SharedPreferences p=a.getSharedPreferences("iss",0);
  TextView label=new TextView(a);label.setText(AppLanguage.text("Vulkan driver · next game start","Driver Vulkan · próximo início"));parent.addView(label);
  String custom=p.getString("custom_driver_name",AppLanguage.text("None imported","Nenhum importado"));
  String[] values={AppLanguage.text("Automatic · detect GPU (default)","Automático · detectar GPU (padrão)"),"Turnip Gen8 V38 Patched · Adreno 8xx","Turnip v27.0.0-R1 · Adreno 6xx / 7xx",AppLanguage.text("System Vulkan driver","Driver Vulkan do sistema"),AppLanguage.text("Imported: ","Importado: ")+custom};
  String[] keys={"auto","gen8","gen7","system","custom"};
  Spinner driver=new Spinner(a);driver.setAdapter(new ArrayAdapter<String>(a,android.R.layout.simple_spinner_dropdown_item,values));
  String selected=selection(a);int index=0;for(int i=0;i<keys.length;i++)if(keys[i].equals(selected))index=i;
  driver.setSelection(index);parent.addView(driver);
  driver.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener(){
   public void onNothingSelected(AdapterView<?> view){}
   public void onItemSelected(AdapterView<?> parent,android.view.View view,int pos,long id){
    if(pos==4&&!customLibrary(a).isFile()){driver.setSelection(0);return;}
    p.edit().putString("bionic_driver",keys[pos]).putInt("driver_schema",2).apply();
   }
  });
  Button pick=new Button(a);pick.setText(AppLanguage.text("Import driver ZIP","Importar driver ZIP"));pick.setOnClickListener(v->choose(a));parent.addView(pick);
  TextView note=new TextView(a);note.setText(AppLanguage.text("Automatic uses Gen8 V38 Patched on Adreno 8xx and v27 R1 on Adreno 6xx/7xx. Other GPUs use the system driver. Restart the game after changing drivers.","Automático usa Gen8 V38 Patched em Adreno 8xx e v27 R1 em Adreno 6xx/7xx. Outras GPUs usam o driver do sistema. Reinicie o jogo após trocar o driver."));parent.addView(note);
 }
 static void choose(Activity a){
  a.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*").putExtra(Intent.EXTRA_MIME_TYPES,new String[]{"application/zip","application/x-zip-compressed","application/octet-stream"}).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION),IMPORT_DRIVER);
 }
 static boolean result(Activity a,int req,int result,Intent data){
  if(req!=IMPORT_DRIVER)return false;
  if(result==Activity.RESULT_OK&&data!=null&&data.getData()!=null)importZip(a,data.getData());
  return true;
 }
 private static byte[] read(InputStream in,int max)throws IOException{
  ByteArrayOutputStream out=new ByteArrayOutputStream();byte[] bytes=new byte[65536];int n,total=0;
  while((n=in.read(bytes))!=-1){total+=n;if(total>max)throw new IOException(AppLanguage.text("Driver file is too large.","Arquivo do driver muito grande."));out.write(bytes,0,n);}return out.toByteArray();
 }
 /** Reject foreign architectures and missing Vulkan HAL exports before selecting the driver. */
 static void validateElf(byte[] data)throws IOException{
  if(data.length<64||data[0]!=127||data[1]!='E'||data[2]!='L'||data[3]!='F'||data[4]!=2||data[5]!=1)throw new IOException(AppLanguage.text("Expected ELF64 little-endian Android driver","Esperado driver Android ELF64 little-endian"));
  ByteBuffer b=ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
  if(b.getShort(16)!=3||b.getShort(18)!=183)throw new IOException(AppLanguage.text("Expected AArch64 shared library","Esperada biblioteca compartilhada AArch64"));
  long table=b.getLong(40);int size=b.getShort(58)&65535,count=b.getShort(60)&65535;
  if(size<64||table<64||count==0||table>data.length-(long)size*count)throw new IOException(AppLanguage.text("Invalid ELF sections","Seções ELF inválidas"));
  boolean hal=false;
  byte[][] accepted={"HMI\0".getBytes(),"vk_icdGetInstanceProcAddr\0".getBytes(),"vkGetInstanceProcAddr\0".getBytes()};
  for(int i=0;i<count;i++){
   int section=(int)(table+(long)i*size);if(b.getInt(section+4)!=11)continue; // SHT_DYNSYM
   long offset=b.getLong(section+24),length=b.getLong(section+32),entry=b.getLong(section+56);int link=b.getInt(section+40);
   if(offset<0||length<0||offset>data.length-length||entry<24||link<0||link>=count)throw new IOException(AppLanguage.text("Invalid driver symbols","Símbolos do driver inválidos"));
   int strings=(int)(table+(long)link*size);long start=b.getLong(strings+24),bytes=b.getLong(strings+32);
   if(start<0||bytes<0||start>data.length-bytes)throw new IOException(AppLanguage.text("Invalid driver strings","Textos do driver inválidos"));
   for(long at=offset;at+24<=offset+length;at+=entry){int name=b.getInt((int)at);if(name<0||name+4>bytes)continue;
    if(b.getShort((int)at+6)==0||(data[(int)at+4]>>>4&15)==0)continue;
    int p=(int)(start+name);
    for(byte[] sym:accepted){
     if(name+sym.length>bytes)continue;
     boolean match=true;for(int k=0;k<sym.length;k++)if(data[p+k]!=sym[k]){match=false;break;}
     if(match){hal=true;break;}
    }
   }
  }
  if(!hal)throw new IOException(AppLanguage.text("Driver does not export the Android Vulkan HAL (HMI)","O driver não exporta a HAL Vulkan Android (HMI)"));
 }
 private static void importZip(Activity a,Uri uri){
  if(!importing.compareAndSet(false,true))return;
  new Thread(()->{String error=null,name=null;File archive=null,temp=null;
   try{
    archive=File.createTempFile("driver-",".zip",a.getCacheDir());
    try(InputStream in=a.getContentResolver().openInputStream(uri)){if(in==null)throw new IOException(AppLanguage.text("Cannot open driver ZIP","Não foi possível abrir o ZIP do driver"));Files.write(archive.toPath(),read(in,32*1024*1024));}
    byte[] library;JSONObject meta=null;
    try(ZipFile zip=new ZipFile(archive)){
     ZipEntry metadata=zip.getEntry("meta.json");
     if(metadata==null)metadata=zip.getEntry("driver.json");
     ZipEntry firstSo=null;
     if(metadata==null){
      java.util.Enumeration<? extends ZipEntry> entries=zip.entries();
      while(entries.hasMoreElements()){
       ZipEntry e=entries.nextElement();if(e.isDirectory())continue;
       String en=e.getName();
       if(metadata==null&&(en.endsWith("/meta.json")||en.endsWith("/driver.json")||"meta.json".equals(en)||"driver.json".equals(en)))metadata=e;
       if(firstSo==null&&en.endsWith(".so"))firstSo=e;
      }
     }
     ZipEntry entry=null;
     if(metadata!=null){
      try(InputStream in=zip.getInputStream(metadata)){meta=new JSONObject(new String(read(in,65536),java.nio.charset.StandardCharsets.UTF_8));}
      if(meta.optInt("minApi",0)>Build.VERSION.SDK_INT)throw new IOException(AppLanguage.text("Driver requires a newer Android version","O driver exige uma versão mais nova do Android"));
      String lib=meta.optString("libraryName","libvulkan_freedreno.so");
      int slash=lib.lastIndexOf('/');if(slash>=0)lib=lib.substring(slash+1);
      if(!lib.endsWith(".so"))throw new IOException(AppLanguage.text("Invalid driver library name","Nome da biblioteca do driver inválido"));
      entry=zip.getEntry(lib);
      if(entry==null){
       String metaName=metadata.getName();int dirSlash=metaName.lastIndexOf('/');
       if(dirSlash>=0)entry=zip.getEntry(metaName.substring(0,dirSlash+1)+lib);
      }
      if(entry==null){
       java.util.Enumeration<? extends ZipEntry> entries=zip.entries();
       while(entries.hasMoreElements()){
        ZipEntry e=entries.nextElement();if(e.isDirectory())continue;
        if(e.getName().endsWith("/"+lib)||e.getName().equals(lib)){entry=e;break;}
        if(firstSo==null&&e.getName().endsWith(".so"))firstSo=e;
       }
      }
      if(entry==null)entry=firstSo;
     }else if(firstSo!=null){
      entry=firstSo;
      String baseName=firstSo.getName();int slash=baseName.lastIndexOf('/');if(slash>=0)baseName=baseName.substring(slash+1);
      meta=new JSONObject().put("name",baseName).put("libraryName","libvulkan_freedreno.so").put("minApi",30);
     }else{
      throw new IOException(AppLanguage.text("Driver ZIP is missing meta.json or a .so library","O ZIP do driver não contém meta.json nem biblioteca .so"));
     }
     if(entry==null)throw new IOException(AppLanguage.text("Driver library is missing","Biblioteca do driver ausente"));
     try(InputStream in=zip.getInputStream(entry)){library=read(in,MAX_LIBRARY);}
    }
    validateElf(library);
    name=meta.optString("name",meta.optString("driverVersion","Custom Turnip Driver")).trim();
    if(name.isEmpty())name="Custom Turnip Driver";
    if(name.length()>160)name=name.substring(0,160);
    meta.put("name",name);meta.put("libraryName","libvulkan_freedreno.so");
    StringBuilder hash=new StringBuilder();for(byte v:MessageDigest.getInstance("SHA-256").digest(library))hash.append(String.format(java.util.Locale.US,"%02x",v&255));
    File root=new File(a.getFilesDir(),"issport/custom-drivers");root.mkdirs();File dest=new File(root,hash.toString());
    if(!dest.isDirectory()){
     temp=Files.createTempDirectory(root.toPath(),"import-").toFile();
     try(FileOutputStream out=new FileOutputStream(new File(temp,"libvulkan_freedreno.so"))){out.write(library);out.getFD().sync();}
     Files.write(new File(temp,"meta.json").toPath(),meta.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
     if(!temp.renameTo(dest))throw new IOException(AppLanguage.text("Cannot install driver","Não foi possível instalar o driver"));temp=null;
    }
    if(!a.getSharedPreferences("iss",0).edit().putString("custom_driver_hash",hash.toString()).putString("custom_driver_name",name).putString("bionic_driver","custom").putInt("driver_schema",2).commit())throw new IOException(AppLanguage.text("Cannot select imported driver","Não foi possível selecionar o driver importado"));
   }catch(Exception e){error=e.getMessage();}
   finally{if(archive!=null)archive.delete();if(temp!=null){for(File f:temp.listFiles())f.delete();temp.delete();}importing.set(false);}
   final String message=error==null?name+AppLanguage.text(" selected. Restart the game to use it."," selecionado. Reinicie o jogo para usá-lo."):error;
   final boolean success=error==null;
   a.runOnUiThread(()->{if(!a.isFinishing()&&!a.isDestroyed())new AlertDialog.Builder(a).setTitle(success?AppLanguage.text("Driver imported","Driver importado"):AppLanguage.text("Import failed","Falha na importação")).setMessage(message).setPositiveButton("OK",null).show();});
  },"iss-driver-import").start();
 }
}

