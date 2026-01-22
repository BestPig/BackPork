# 6.xx Notes

Some games needs an extra patch on `sce_module/libc.prx` to work.

```bash
perl -0777 -i -pe 's/4h6F1LLbTiw#A#B/IWIBBdTHit4#A#B/g' sce_module/libc.prx
```
