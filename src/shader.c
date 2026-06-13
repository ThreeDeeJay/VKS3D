*** Begin Patch
*** Update File: src/shader.c
@@
-#include "stereo_icd.h"
+#include "stereo_icd.h"
+#include "spirv_min.h"
@@
-static inline uint32_t op_(uint32_t op, uint32_t wc) { return (wc<<16)|op; }
+static inline uint32_t op_(uint32_t op, uint32_t wc) { return (wc<<16)|op; }
@@
-    for (size_t i = 5; i < m->words; ) {
-        uint32_t op = m->w[i] & 0xffff, wc = m->w[i] >> 16;
-        if (!wc || i + wc > m->words) break;
-        switch (op) {
-        case 17: /* OpCapability */
-            if (wc >= 2 && m->w[i+1] == 5296) m->has_mv_cap = true;
-            break;
+    for (size_t i = 5; i < m->words; ) {
+        uint32_t op = m->w[i] & 0xffff, wc = m->w[i] >> 16;
+        if (!wc || i + wc > m->words) break;
+        switch (op) {
+        case 17: /* OpCapability */
+            if (wc >= 2 && m->w[i+1] == SpvCapabilityMultiView) m->has_mv_cap = true;
+            break;
@@
-            if (!mv_done && need_mv_cap) {
-                uint32_t c[]={op_(17,2),5296};
-                sb_push_n(&ob,c,2); mv_done=true; }
+            if (!mv_done && need_mv_cap) {
+                uint32_t c[]={op_(17,2),SpvCapabilityMultiView};
+                sb_push_n(&ob,c,2); mv_done=true; }
*** End Patch
