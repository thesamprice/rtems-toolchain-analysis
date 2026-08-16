/*
 * canceltrace v2: non-perturbing QEMU TCG plugin tracing the MicroBlaze
 * pthread-cancellation flow, flush-safe (writes each event to logf= with
 * fflush). Watches user-space (static cancel-min) addresses passed as args.
 * Also reads cancelhandling (r21 + choff) in the SIGCANCEL handler.
 */
#include <glib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAXW 12
static uint64_t addr[MAXW]; static const char *name[MAXW]; static int nw;
static uint64_t BSTART, BEND;
static int CTXOFF = 148;
static int CHOFF = -1056;                 /* cancelhandling offset from r21 */
static struct qemu_plugin_register *h_r7, *h_r5, *h_r21, *h_r6, *h_r3, *h_r19, *h_r1;
static FILE *lf;

static struct qemu_plugin_register *find_reg(const char *want)
{
    g_autoptr(GArray) regs = qemu_plugin_get_registers();
    for (guint i = 0; i < regs->len; i++) {
        qemu_plugin_reg_descriptor *rd =
            &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        g_autofree gchar *lo = g_utf8_strdown(rd->name, -1);
        if (!strcmp(lo, want)) return rd->handle;
    }
    return NULL;
}
static void vcpu_init(qemu_plugin_id_t id, unsigned int v)
{
    if (!h_r7) h_r7 = find_reg("r7");
    if (!h_r5) h_r5 = find_reg("r5");
    if (!h_r6) h_r6 = find_reg("r6");
    if (!h_r3) h_r3 = find_reg("r3");
    if (!h_r19) h_r19 = find_reg("r19");
    if (!h_r1) h_r1 = find_reg("r1");
    if (!h_r21) h_r21 = find_reg("r21");
}
static uint32_t rd_reg(struct qemu_plugin_register *h)
{
    g_autoptr(GByteArray) b = g_byte_array_new();
    uint32_t v = 0; qemu_plugin_read_register(h, b);
    if (b->len >= 4) memcpy(&v, b->data, 4);
    return v;
}
static uint32_t rd_mem(uint64_t a)
{
    g_autoptr(GByteArray) b = g_byte_array_new();
    uint32_t v = 0xdeadbeef;
    if (qemu_plugin_read_memory_vaddr(a, b, 4) && b->len >= 4) memcpy(&v, b->data, 4);
    return v;
}
static void emit(const char *s){ if(lf){ fputs(s,lf); fputc('\n',lf); fflush(lf);} }

static void cb(unsigned int vcpu, void *ud)
{
    intptr_t idx = (intptr_t)ud;
    if (!strcmp(name[idx], "sigcancel_handler")) {
        uint32_t sig = h_r5 ? rd_reg(h_r5) : 0;
        uint32_t ctx = h_r7 ? rd_reg(h_r7) : 0;
        uint32_t fpc = ctx ? rd_mem(ctx + CTXOFF) : 0;
        uint32_t r21 = h_r21 ? rd_reg(h_r21) : 0;
        uint32_t ch  = r21 ? rd_mem(r21 + CHOFF) : 0;
        uint32_t si  = h_r6 ? rd_reg(h_r6) : 0;
        uint32_t sicode = si ? rd_mem(si + 8) : 0;
        uint32_t sipid  = si ? rd_mem(si + 12) : 0;
        int inr = (fpc >= BSTART && fpc < BEND);
        int enc = ((ch & 0x3B) == 0x0A);   /* async gate */
        g_autofree char *s = g_strdup_printf(
            "TRACE %s sig=%u framepc=%08x inrange=%d cancelhandling=%08x async_gate=%d "
            "si_code=%d si_pid=%u (SI_TKILL=-6)",
            name[idx], sig, fpc, inr, ch, enc, (int32_t)sicode, sipid);
        emit(s);
    } else if (!strncmp(name[idx], "H_sipid", 7)) {
        uint32_t r1 = h_r1 ? rd_reg(h_r1) : 0;
        uint32_t r6 = h_r6 ? rd_reg(h_r6) : 0;
        uint32_t sipid = r6 ? rd_mem(r6 + 12) : 0;
        uint32_t r19 = h_r19 ? rd_reg(h_r19) : 0;
        uint32_t r3  = h_r3 ? rd_reg(h_r3) : 0;
        g_autofree char *s = g_strdup_printf(
            "TRACE %s@%08x sp=%08x si=%08x mem[si+12]=%08x r19=%08x r3=%08x",
            name[idx], (unsigned)addr[idx], r1, r6, sipid, r19, r3);
        emit(s);
    } else if (!strcmp(name[idx], "H_regs")) {
        uint32_t r1 = h_r1 ? rd_reg(h_r1) : 0;
        uint32_t r6 = h_r6 ? rd_reg(h_r6) : 0;   /* si */
        uint32_t r7 = h_r7 ? rd_reg(h_r7) : 0;   /* ctx */
        uint32_t r19 = h_r19 ? rd_reg(h_r19) : 0;
        g_autofree char *s = g_strdup_printf(
            "TRACE H_regs sp(r1)=%08x si(r6)=%08x ctx(r7)=%08x r19_in=%08x "
            "si-sp=%d si+12_vs_sp+28=%s",
            r1, r6, r7, r19, (int)(r6 - r1),
            (r6 + 12 == r1 + 28) ? "OVERLAP" : "distinct");
        emit(s);
    } else if (!strncmp(name[idx], "H_r19", 5)) {
        uint32_t r19 = h_r19 ? rd_reg(h_r19) : 0;
        g_autofree char *s = g_strdup_printf("TRACE H_r19@%08x r19=%u (0x%08x)",
            (unsigned)addr[idx], r19, r19);
        emit(s);
    } else if (!strcmp(name[idx], "H_pidcheck")) {
        uint32_t getpid_ret = h_r3 ? rd_reg(h_r3) : 0;   /* r3 = getpid() return */
        uint32_t si_pid = h_r19 ? rd_reg(h_r19) : 0;     /* r19 = si->si_pid */
        g_autofree char *s = g_strdup_printf(
            "TRACE H_pidcheck getpid_ret=%u si_pid=%u match=%d",
            getpid_ret, si_pid, getpid_ret == si_pid);
        emit(s);
    } else {
        g_autofree char *s = g_strdup_printf("TRACE %s entered", name[idx]);
        emit(s);
    }
}
static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, i);
        uint64_t va = qemu_plugin_insn_vaddr(in);
        for (int w = 0; w < nw; w++)
            if (va == addr[w])
                qemu_plugin_register_vcpu_insn_exec_cb(in, cb,
                    QEMU_PLUGIN_CB_R_REGS, (void*)(intptr_t)w);
    }
}
static uint64_t geta(const char*v){ return v?g_ascii_strtoull(v,NULL,0):0; }

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
    const qemu_info_t *info, int argc, char **argv)
{
    const char *logf = "/tmp/canceltrace.out";
    for (int i=0;i<argc;i++){ char*k=argv[i],*e=strchr(k,'='); if(!e)continue;*e=0;char*val=e+1;
        if(!strcmp(k,"bstart"))BSTART=geta(val);
        else if(!strcmp(k,"bend"))BEND=geta(val);
        else if(!strcmp(k,"ctxoff"))CTXOFF=(int)geta(val);
        else if(!strcmp(k,"choff"))CHOFF=(int)geta(val);
        else if(!strcmp(k,"logf"))logf=val;
        else if(nw<MAXW){ name[nw]=g_strdup(k); addr[nw]=geta(val); nw++; }
    }
    lf = fopen(logf, "w");
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    return 0;
}
