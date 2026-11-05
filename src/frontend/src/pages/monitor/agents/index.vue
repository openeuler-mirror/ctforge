<template>
  <t-card title="主机列表" :bordered="false">
    <template #actions>
      <t-button theme="primary" :loading="loading" @click="fetchData">刷新</t-button>
    </template>
    <t-table
      :data="agents"
      :columns="columns"
      :loading="loading"
      row-key="agent_id"
      hover
      @row-click="onRowClick"
    >
      <template #hostname="{ row }">
        <div class="host-cell">
          <span v-if="row.remark" class="host-remark" :title="row.hostname">{{ row.remark }}</span>
          <span v-else>{{ row.hostname }}</span>
          <span v-if="row.remark" class="host-real">{{ row.hostname }}</span>
        </div>
      </template>
      <template #os="{ row }">
        <t-tooltip :content="osTooltip(row)">
          <span class="os-cell">{{ osDisplay(row) }}</span>
        </t-tooltip>
      </template>
      <template #online="{ row }">
        <t-tag v-if="row.online" theme="success" variant="light">在线</t-tag>
        <t-tooltip v-else-if="row.discovered" content="局域网发现的主机，尚未接入控制通道">
          <t-tag theme="warning" variant="light">待接入</t-tag>
        </t-tooltip>
        <t-tag v-else theme="default" variant="light">离线</t-tag>
      </template>
      <template #metrics="{ row }">
        <template v-if="row.last_heartbeat">
          <div class="metric-cell">
            <span>内存</span>
            <t-progress :percentage="round(row.last_heartbeat.mem_percent)" :color="percentColor(row.last_heartbeat.mem_percent)" size="small" :label="false" style="width:80px" />
            <span class="metric-num">{{ round(row.last_heartbeat.mem_percent) }}%</span>
          </div>
          <div class="metric-cell">
            <span>磁盘</span>
            <t-progress :percentage="round(row.last_heartbeat.disk_percent)" :color="percentColor(row.last_heartbeat.disk_percent)" size="small" :label="false" style="width:80px" />
            <span class="metric-num">{{ round(row.last_heartbeat.disk_percent) }}%</span>
          </div>
        </template>
        <span v-else-if="row.discovered" class="discover-hint">已通过局域网发现</span>
        <span v-else>-</span>
      </template>
      <template #load="{ row }">
        {{ row.last_heartbeat ? row.last_heartbeat.load1.toFixed(2) : '-' }}
      </template>
      <template #hb="{ row }">
        <div class="hb-bar" :title="hbTitle(row)">
          <span
            v-for="(state, i) in hbStates(row)"
            :key="i"
            class="hb-cell"
            :class="state"
          ></span>
        </div>
      </template>
      <template #latency="{ row }">
        <span v-if="row.online && row.latency_ms != null" :class="latencyClass(row.latency_ms)">
          {{ row.latency_ms.toFixed(0) }}ms
        </span>
        <span v-else>-</span>
      </template>
      <template #net="{ row }">
        <div v-if="row.online && (row.net_rx_rate || row.net_tx_rate)" class="net-cell">
          <span class="net-rx">↓{{ formatRate(row.net_rx_rate) }}</span>
          <span class="net-tx">↑{{ formatRate(row.net_tx_rate) }}</span>
        </div>
        <span v-else-if="row.online && row.net_conns" class="net-conn">{{ row.net_conns }} 连接</span>
        <span v-else>-</span>
      </template>
      <template #last_seen="{ row }">
        {{ formatTime(row.last_seen) }}
      </template>
      <template #op="{ row }">
        <t-space v-if="row.discovered && !row.online" break-line>
          <span class="discover-hint">等待 agent 连接</span>
        </t-space>
        <t-dropdown v-else trigger="click" :options="opMenuOptions(row)" @click="(v: any) => onOpMenuClick(v, row)">
          <t-button size="small" variant="outline" theme="primary" @click.stop>
            操作 <t-icon name="chevron-down" size="12px" />
          </t-button>
        </t-dropdown>
      </template>
    </t-table>
  </t-card>
</template>

<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue';
import { useRouter } from 'vue-router';
import { DialogPlugin, MessagePlugin } from 'tdesign-vue-next';
import type { PrimaryTableCol } from 'tdesign-vue-next';
import dayjs from 'dayjs';

import { http, listAgents, type Agent } from '@/api/agent';

const router = useRouter();
const agents = ref<Agent[]>([]);
const loading = ref(false);
let timer: number | undefined;

const columns: PrimaryTableCol[] = [
  { colKey: 'online', title: '状态', width: 80 },
  { colKey: 'hostname', title: '主机名/备注', width: 200, ellipsis: true },
  { colKey: 'ip', title: 'IP', width: 130 },
  { colKey: 'os', title: '系统', width: 200, ellipsis: true },
  { colKey: 'arch', title: '架构', width: 90 },
  { colKey: 'metrics', title: '资源', width: 200 },
  { colKey: 'load', title: '负载(1m)', width: 90 },
  { colKey: 'latency', title: '延迟', width: 80 },
  { colKey: 'hb', title: '心跳', width: 110 },
  { colKey: 'net', title: '网络', width: 140 },
  { colKey: 'version', title: '版本', width: 80 },
  { colKey: 'last_seen', title: '最后在线', width: 150 },
  { colKey: 'op', title: '操作', width: 170, fixed: 'right' },
];

/** 操作菜单选项 */
function opMenuOptions(row: any) {
  const base = [
    { content: '详情', value: 'detail' },
    { content: '备注', value: 'remark' },
    { content: '终端', value: 'terminal' },
    { content: '文件', value: 'files' },
    { content: '配置', value: 'config' },
    { content: '改IP', value: 'changeIP' },
    { content: '升级', value: 'upgrade' },
    { content: '卸载', value: 'uninstall' },
  ];
  // 离线主机支持直接删除（从服务端移除，不再出现在列表中）
  if (!row.online) {
    base.push({ content: '删除', value: 'delete' });
  }
  return base;
}

/** 操作菜单点击 */
function onOpMenuClick(v: any, row: any) {
  const act = v?.value || v;
  switch (act) {
    case 'detail': goDetail(row); break;
    case 'remark': onSetRemark(row); break;
    case 'terminal': goTerminal(row); break;
    case 'files': goFiles(row); break;
    case 'config': goConfig(row); break;
    case 'changeIP': onChangeIP(row); break;
    case 'upgrade': onRemoteUpgrade(row); break;
    case 'uninstall': onRemoteUninstall(row); break;
    case 'delete': onDeleteOffline(row); break;
  }
}

/** 设置主机备注名 */
async function onSetRemark(row: Agent) {
  const name = window.prompt('主机备注名（留空清除备注）：', row.remark || '');
  if (name === null) return;
  try {
    const { setAgentRemark } = await import('@/api/agent');
    await setAgentRemark(row.agent_id, name.trim());
    MessagePlugin.success(name.trim() ? '已设置备注' : '已清除备注');
    fetchData();
  } catch (e: any) {
    MessagePlugin.error(e.message || '设置备注失败');
  }
}

/** 删除离线主机（从服务端移除） */
async function onDeleteOffline(row: Agent) {
  const { DialogPlugin } = await import('tdesign-vue-next');
  const dlg = DialogPlugin.confirm({
    header: '删除离线主机',
    body: `确定从服务端删除 ${row.hostname} (${row.ip}) 吗？此操作不可恢复，该主机将不再出现在列表中。`,
    confirmBtn: '删除', cancelBtn: '取消',
    onConfirm: async () => {
      dlg.destroy();
      try {
        await fetch(`/api/agents/${encodeURIComponent(row.agent_id)}`, {
          method: 'DELETE',
          headers: { Authorization: 'Bearer ' + localStorage.getItem('ctc_token') },
        });
        MessagePlugin.success('已删除');
        fetchData();
      } catch (e: any) {
        MessagePlugin.error(e.message || '删除失败');
      }
    },
    onCancel: () => { dlg.destroy(); },
  });
}

function round(v: number) {
  return Math.round(v * 10) / 10;
}

function percentColor(v: number) {
  if (v >= 90) return '#e34d59';
  if (v >= 70) return '#ed7b2f';
  return '#0594fa';
}

/** 心跳图形：把最近心跳时间戳转成状态格 */
function hbStates(row: any): string[] {
  const hist: number[] = row.hb_history || [];
  const now = Date.now();
  const interval = 10000; // 期望 10s 一次
  const result: string[] = [];
  const slots = 20;
  // 以 now 为终点，向前推 slots 个时间槽
  for (let i = slots - 1; i >= 0; i--) {
    const slotStart = now - (i + 1) * interval;
    const slotEnd = now - i * interval;
    const hit = hist.some((t) => t >= slotStart && t < slotEnd);
    if (hit) result.push('ok');
    else if (i === 0) result.push('pending');
    else result.push('miss');
  }
  return result;
}

function hbTitle(row: any) {
  if (!row.online) return '离线';
  const hist = row.hb_history || [];
  if (!hist.length) return '无心跳记录';
  const last = hist[hist.length - 1];
  const ago = Math.round((Date.now() - last) / 1000);
  return `最近心跳 ${ago}s 前，共 ${hist.length} 条记录`;
}

function latencyClass(ms: number) {
  if (ms < 50) return 'lat-ok';
  if (ms < 200) return 'lat-mid';
  return 'lat-bad';
}

function formatRate(bps?: number) {
  if (bps == null || bps === 0) return '0';
  if (bps > 1024 * 1024) return (bps / 1024 / 1024).toFixed(1) + 'M/s';
  if (bps > 1024) return (bps / 1024).toFixed(1) + 'K/s';
  return bps.toFixed(0) + 'B/s';
}

/** 系统显示：优先详细版本，否则降级 */
function osDisplay(row: any): string {
  const full = row.os_version;
  if (full) {
    // 从 "Fedora Linux 44 (Server Edition)" 提取 "Fedora 44 Server"
    const m = full.match(/^(Fedora|Ubuntu|CTyunOS|CentOS|Rocky|AlmaLinux|Debian|openSUSE|SLES|Alpine|Kylin|UOS|EulerOS|Anolis|OpenCloudOS)(?:\s+Linux)?\s*([\d.]+)?/i);
    if (m) {
      const distro = m[1];
      const ver = m[2] || '';
      const short = (distro + ' ' + ver).trim();
      return short.length > 24 ? short.slice(0, 21) + '...' : short;
    }
    return full.length > 24 ? full.slice(0, 21) + '...' : full;
  }
  return row.os || '-';
}

function osTooltip(row: any): string {
  const parts = [row.os_version, row.kernel ? `内核 ${row.kernel}` : ''].filter(Boolean);
  return parts.join(' · ') || row.os || '-';
}

function formatTime(t: string) {
  return t ? dayjs(t).format('MM-DD HH:mm:ss') : '-';
}

async function fetchData() {
  loading.value = true;
  try {
    agents.value = await listAgents();
  } finally {
    loading.value = false;
  }
}

function withAgent(path: string, row: Agent) {
  router.push({ path, query: { agent: row.agent_id, hostname: row.hostname } });
}

const onRowClick = (context: any) => {
  // 点击操作列（按钮/下拉）不进入详情
  const target = context.e?.target as HTMLElement | null;
  if (target?.closest('.t-dropdown, .t-button, .t-link, .t-icon')) return;
  goDetail(context.row as Agent);
};
const goDetail = (row: Agent) => withAgent('/monitor/detail', row);
const goTerminal = (row: Agent) => withAgent('/ops/terminal', row);
const goFiles = (row: Agent) => withAgent('/ops/files', row);
const goConfig = (row: Agent) => withAgent('/ops/config', row);

/** 修改服务端 IP（让客户端连到新地址） */
async function onChangeIP(row: Agent) {
  const curIP = row.ip || '';
  const newIP = window.prompt(
    `修改服务端 IP · ${row.hostname}\n当前 IP：${curIP}\n输入新的服务端地址：`,
    curIP,
  );
  if (!newIP || newIP === curIP) return;
  try {
    await http.post(`/agents/${row.agent_id}/server-ip`, {
      server_host: newIP.trim(),
      server_port: 9000,
    });
    MessagePlugin.success(`已通知客户端切换到 ${newIP.trim()}，重连后生效`);
  } catch (e: any) {
    MessagePlugin.error(e.message || '操作失败');
  }
}

/** 远程升级（从服务端拉取新版本） */
async function onRemoteUpgrade(row: Agent) {
  const dlg = DialogPlugin.confirm({
    header: `远程升级 · ${row.hostname}`,
    body: '将从服务端拉取最新版本的 agent 并替换，升级后客户端会自动重启。',
    onConfirm: async () => {
      try {
        await http.post(`/agents/${row.agent_id}/upgrade`, {});
        MessagePlugin.success('升级命令已下发');
        fetchData();
      } catch (e: any) {
        MessagePlugin.error(e.message || '升级失败');
      }
      dlg.destroy();
    },
  });
}

/** 远程卸载（从列表删除并清理客户端文件） */
async function onRemoteUninstall(row: Agent) {
  const dlg = DialogPlugin.confirm({
    header: `远程卸载 · ${row.hostname}`,
    body: `将彻底卸载该客户端：\n1. 从主机列表删除\n2. 在客户端主机删除 agent 二进制与配置文件\n此操作不可恢复！`,
    theme: 'danger',
    onConfirm: async () => {
      try {
        await http.post(`/agents/${row.agent_id}/remote-uninstall`, {});
        MessagePlugin.success('卸载命令已下发，客户端将被清除');
        setTimeout(fetchData, 3000);
      } catch (e: any) {
        MessagePlugin.error(e.message || '卸载失败');
      }
      dlg.destroy();
    },
  });
}

onMounted(() => {
  fetchData();
  timer = window.setInterval(fetchData, 10000);
});
onUnmounted(() => window.clearInterval(timer));
</script>

<style scoped>
.danger-item {
  color: var(--td-error-color) !important;
}
.os-cell {
  font-size: 12px;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  display: block;
}
.metric-cell {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 12px;
  line-height: 20px;
}
.metric-num {
  color: var(--td-text-color-secondary);
  width: 44px;
}
.host-cell { display: flex; flex-direction: column; line-height: 1.3; }
.host-remark { font-weight: 600; }
.host-real { font-size: 11px; color: var(--td-text-color-secondary); }
.discover-hint {
  font-size: 12px;
  color: var(--td-text-color-placeholder);
}
.hb-bar {
  display: flex;
  gap: 1px;
  align-items: center;
}
.hb-cell {
  width: 4px;
  height: 14px;
  border-radius: 1px;
  background: #e5e5e5;
}
.hb-cell.ok { background: #2ba471; }
.hb-cell.miss { background: #e34d59; }
.hb-cell.pending { background: #dcdcdc; }
.lat-ok { color: #2ba471; }
.lat-mid { color: #ed7b2f; }
.lat-bad { color: #e34d59; }
.net-cell {
  display: flex;
  flex-direction: column;
  font-size: 11px;
  line-height: 16px;
}
.net-rx { color: #0594fa; }
.net-tx { color: #00a870; }
.net-conn {
  font-size: 12px;
  color: var(--td-text-color-secondary);
}
</style>
