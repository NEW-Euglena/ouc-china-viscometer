const fs=require('node:fs'), vm=require('node:vm'), assert=require('node:assert/strict');
const html=fs.readFileSync(require('node:path').join(__dirname,'dashboard.html'),'utf8');
const extract=name=>vm.runInNewContext(html.match(new RegExp('function '+name+'\\([^]*?\\n\\}'))[0]+';'+name);
const viscosity=extract('calculateViscosity'), mass=extract('calculateMolecularWeight');
const close=(a,b,t=1e-8)=>assert.ok(Math.abs(a-b)<t,`${a} != ${b}`);
close(viscosity(120,100,.5).intrinsic,.376068538498453750015375731669811,1e-12);
close(mass(.98).value,350000);
close(mass(3.96).value,3020000);
close(mass(2.63).value,1605864.733273972);
close(Math.abs(mass(2.63).value-1600000)/1600000*100,.36654582962324,1e-9);
for(const x of [0,-1,NaN,Infinity]) {
  assert.ok(viscosity(x,100,.5).error);
  assert.ok(viscosity(120,x,.5).error);
  assert.ok(viscosity(120,100,x).error);
  assert.ok(mass(x).error);
}
assert.ok(viscosity(90,100,.5).error);
assert.equal(viscosity(100,100,.5).intrinsic,0);
assert.ok(mass(0).error);
assert.ok(viscosity(1+1e-8,1,1).intrinsic>0);
assert.ok(mass(1e-8).note.includes('extrapolated'));
assert.ok(mass(Number.MAX_VALUE).error);
console.log('通过：单点法标准值、零值、无效输入、接近1、建模两点、独立测试样品及范围提示。');
