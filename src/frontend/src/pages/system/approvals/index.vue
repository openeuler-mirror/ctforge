<template>
  <div>
    <!-- 待批准主机 -->
    <t-card title="待批准主机" :bordered="false">
      <template #actions>
        <t-button theme="primary" :loading="loading" @click="fetchAll">刷新</t-button>
      </template>
      <t-alert v-if="pendings.length === 0" theme="info" style="margin-bottom: 16px">
        暂无待批准的主机。agent 在局域网中发现服务端后会自动出现在这里。
      </t-alert>
      <t-table v-else :data="pendings" :columns="columns" row-key="agent_id" hover>
        <template #sys="{ row }">
          <div class="sys-cell">
            <div>{{ row.os_version || row.os || '-' }}</div>
            <div class="kernel">{{ row.kernel || '-' }}</div>
          </div>
        </template>
        <template #disk="{ row }">
          <t-progress v-if="row.disk_percent != null" :percentage="Math.round(row.disk_percent)" size="small" :label="true" style="width: 100px" />
          <span v-else>-</span>
        </template>
        <template #last_seen="{ row }">{{ formatTime(row.last_seen) }}</template>
        <template #op="{ row }">
          <t-space break-line>
            <t-link theme="success" hover="color" @click="onApprove(row)">批准</t-link>
            <t-link theme="danger" hover="color" @click="onReject(row)">拒绝</t-link>
          </t-space>
        </template>
      </t-table>
    </t-card>

    <!-- 已批准主机 -->
    <t-card title="已批准主机" :bordered="false" style="margin-top: 16px">
      <t-table :data="approved" :columns="approvedColumns" row-key="agent_id" hover>
        <template #online="{ row }">
          <t-tag :theme="row.online ? 'success' : 'default'" variant="light">
            {{ row.online ? '在线' : '离线' }}
          </t-tag>
        </template>
        <template #last_seen="{ row }">{{ formatTime(row.last_seen) }}</template>
      </t-table>
    </t-card>

    <!-- 卸载请求 -->
    <t-card title="卸载请求" :bordered="false" style="margin-top: 16px">
      <t-alert v-if="uninstalls.length === 0" theme="info" style="margin-bottom: 16px">
        暂无卸载请求。客户端执行 `ctforge-agent --uninstall` 后会出现在这里。
      </t-alert>
      <t-table v-else :data="uninstalls" :columns="uninstallColumns" row-key="agent_id" hover>
        <template #time="{ row }">{{ formatTime(row.at) }}</template>
        <template #status="{ row }">
          <t-tag v-if="row.decided_at" :theme="row.approved ? 'success' : 'danger'" variant="light">
            {{ row.approved ? '已批准' : '已拒绝' }}
          </t-tag>
          <t-tag v-else theme="warning" variant="light">待审批</t-tag>
        </template>
        <template #op="{ row }">
          <t-space v-if="!row.decided_at" break-line>
            <t-link theme="success" hover="color" @click="onUninstall(row, true)">批准卸载</t-link>
            <t-link theme="danger" hover="color" @click="onUninstall(row, false)">拒绝</t-link>
          </t-space>
          <span v-else>-</span>
        </template>
      </t-table>
    </t-card>

    <!-- 部署命令 -->
    <t-card title="一键部署" :bordered="false" style="margin-top: 16px">
      <t-alert theme="success" style="margin-bottom: 12px">
        在目标 Linux 主机上以管理员身份执行以下命令即可完成安装并自动纳管。
      </t-alert>

      <!-- 服务端地址配置 -->
      <t-form :data="deployForm" layout="inline" style="margin-bottom: 12px">
        <t-form-item label="服务端地址">
          <t-input
            v-model="deployForm.serverHost"
            placeholder="留空 = 自动检测"
            style="width: 220px"
            :tips="serverIps.length > 0 ? `可用 IP: ${serverIps.join(', ')}` : ''"
          />
        </t-form-item>
        <t-form-item>
          <t-button
            theme="primary"
            size="small"
            :loading="savingDeploy"
            @click="saveDeployConfig"
          >保存</t-button>
          <t-button
            v-if="deployForm.serverHost"
            theme="default"
            size="small"
            style="margin-left: 8px"
            @click="resetDeployConfig"
          >恢复自动</t-button>
        </t-form-item>
      </t-form>
      <t-alert
        v-if="serverIps.length > 1"
        theme="info"
        style="margin-bottom: 12px"
      >
        检测到 {{ serverIps.length }} 个可用 IP，agent 将自动切换：{{ serverIps.join(' → ') }}
      </t-alert>

      <div class="cmd-box">
        <code>{{ deployCmd }}</code>
        <t-button theme="primary" size="small" @click="copyCmd">复制</t-button>
      </div>
      <div class="cmd-hint">
        服务端 ID：{{ serverId }}<br>
        安装后 agent 会出现在"待批准"列表中，批准后自动上线。
      </div>
    </t-card>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, reactive, ref } from 'vue';
import { MessagePlugin, DialogPlugin } from 'tdesign-vue-next';
import type { PrimaryTableCol } from 'tdesign-vue-next';
import dayjs from 'dayjs';

import {
  approveAgent, rejectAgent, decideUninstall, getDeployInfo, listAgents, listApprovals, listUninstallRequests,
  type Agent, type PendingAgent, type UninstallReq,
} from '@/api/agent';
import { http } from '@/api/agent';

const pendings = ref<PendingAgent[]>([]);
const approved = ref<Agent[]>([]);
const uninstalls = ref<UninstallReq[]>([]);
const serverId = ref('');
const deployCmd = ref('');
const loading = ref(false);
const serverIps = ref<string[]>([]);
const savingDeploy = ref(false);
const deployForm = reactive({ serverHost: '' });
let timer: number | undefined;

// ---------- 部署配置 ----------
async function fetchDeployConfig() {
  try {
    const res = await http.get<never, {
      server_ips: string[];
      custom_host: string;
      auto_detect_host: string;
      download_url: string;
    }>('/deploy/config');
    serverIps.value = res.server_ips || [];
    deployForm.serverHost = res.custom_host || '';
  } catch { /* 静默失败，不影响主要功能 */ }
}

async function saveDeployConfig() {
  savingDeploy.value = true;
  try {
    await http.put('/deploy/config', { server_host: deployForm.serverHost });
    MessagePlugin.success('服务端地址已保存');
    await fetchAll();
  } catch (e: unknown) {
    MessagePlugin.error(`保存失败: ${(e as Error).message}`);
  } finally {
    savingDeploy.value = false;
  }
}

async function resetDeployConfig() {
  deployForm.serverHost = '';
  await saveDeployConfig();
}

const columns: PrimaryTableCol[] = [
  { colKey: 'agent_id', title: '主机标识', width: 180, ellipsis: true },
  { colKey: 'ip', title: 'IP', width: 130 },
  { colKey: 'sys', title: '系统', width: 240 },
  { colKey: 'disk', title: '磁盘占用', width: 130 },
  { colKey: 'version', title: 'Agent 版本', width: 100 },
  { colKey: 'last_seen', title: '最后出现', width: 160 },
  { colKey: 'op', title: '操作', width: 140, fixed: 'right' },
];

const approvedColumns: PrimaryTableCol[] = [
  { colKey: 'agent_id', title: '主机标识', width: 180, ellipsis: true },
  { colKey: 'ip', title: 'IP', width: 130 },
  { colKey: 'online', title: '状态', width: 90 },
  { colKey: 'os_version', title: '系统', width: 220, ellipsis: true },
  { colKey: 'kernel', title: '内核', width: 200, ellipsis: true },
  { colKey: 'last_seen', title: '最后在线', width: 160 },
];

const uninstallColumns: PrimaryTableCol[] = [
  { colKey: 'agent_id', title: '主机标识', width: 180 },
  { colKey: 'hostname', title: '主机名', width: 160 },
  { colKey: 'time', title: '请求时间', width: 160 },
  { colKey: 'reason', title: '原因', ellipsis: true },
  { colKey: 'status', title: '状态', width: 100 },
  { colKey: 'op', title: '操作', width: 150, fixed: 'right' },
];

function formatTime(t?: string | number) {
  if (!t) return '-';
  const n = typeof t === 'number' ? t : new Date(t).getTime();
  return n > 0 ? dayjs(n).format('MM-DD HH:mm:ss') : '-';
}

async function fetchAll() {
  loading.value = true;
  try {
    const [appr, agents, unins] = await Promise.all([
      listApprovals(),
      listAgents(),
      listUninstallRequests(),
    ]);
    pendings.value = appr.pending || [];
    serverId.value = appr.server_id || '';
    approved.value = agents.filter((a) => a.approval === 'approved');
    uninstalls.value = unins || [];
  } finally {
    loading.value = false;
  }
}

async function fetchDeploy() {
  try {
    const info = await getDeployInfo();
    deployCmd.value = info.command;
    if (info.server_id) serverId.value = info.server_id;
  } catch { /* 忽略 */ }
}

function onApprove(row: PendingAgent) {
  const dlg = DialogPlugin.confirm({
    header: '批准接入',
    body: `确定批准 ${row.hostname} (${row.ip}) 接入吗？批准后该主机将开始上报监控数据。`,
    onConfirm: async () => {
      try {
        await approveAgent(row.agent_id);
        MessagePlugin.success('已批准');
        fetchAll();
      } catch (e: any) {
        MessagePlugin.error(e.message || '操作失败');
      }
      dlg.destroy();
    },
  });
}

function onReject(row: PendingAgent) {
  const dlg = DialogPlugin.confirm({
    header: '拒绝接入',
    body: `确定拒绝 ${row.hostname} (${row.ip}) 吗？拒绝后该主机将无法连接服务端。`,
    theme: 'warning',
    onConfirm: async () => {
      try {
        await rejectAgent(row.agent_id);
        MessagePlugin.success('已拒绝');
        fetchAll();
      } catch (e: any) {
        MessagePlugin.error(e.message || '操作失败');
      }
      dlg.destroy();
    },
  });
}

function onUninstall(row: UninstallReq, approved: boolean) {
  const dlg = DialogPlugin.confirm({
    header: approved ? '批准卸载' : '拒绝卸载',
    body: approved
      ? `确定批准 ${row.hostname} 卸载 agent 吗？批准后 agent 将自动停止并删除自身。`
      : `确定拒绝 ${row.hostname} 的卸载请求吗？`,
    theme: approved ? 'warning' : 'default',
    onConfirm: async () => {
      try {
        await decideUninstall(row.agent_id, approved);
        MessagePlugin.success(approved ? '已批准卸载' : '已拒绝');
        fetchAll();
      } catch (e: any) {
        MessagePlugin.error(e.message || '操作失败');
      }
      dlg.destroy();
    },
  });
}

async function copyCmd() {
  const text = deployCmd.value;
  if (!text) {
    MessagePlugin.warning('命令尚未加载');
    return;
  }
  // 1) 优先用 Clipboard API（仅 HTTPS / localhost 可用）
  if (navigator.clipboard && window.isSecureContext) {
    try {
      await navigator.clipboard.writeText(text);
      MessagePlugin.success('已复制到剪贴板');
      return;
    } catch {
      /* 回退 */
    }
  }
  // 2) HTTP 环境回退：临时 textarea + execCommand
  try {
    const ta = document.createElement('textarea');
    ta.value = text;
    ta.style.cssText = 'position:fixed;left:-9999px;top:0;opacity:0;';
    ta.setAttribute('readonly', '');
    document.body.appendChild(ta);
    ta.select();
    ta.setSelectionRange(0, text.length);
    const ok = document.execCommand('copy');
    document.body.removeChild(ta);
    if (ok) {
      MessagePlugin.success('已复制到剪贴板');
    } else {
      // 3) 最终回退：选中命令文本让用户手动 Ctrl+C
      const el = document.querySelector('.cmd-box code') as HTMLElement | null;
      if (el) {
        const range = document.createRange();
        range.selectNodeContents(el);
        const sel = window.getSelection();
        sel?.removeAllRanges();
        sel?.addRange(range);
      }
      MessagePlugin.warning('已选中命令，请按 Ctrl+C 复制');
    }
  } catch {
    MessagePlugin.error('复制失败，请手动选择命令复制');
  }
}

onMounted(() => {
  fetchAll();
  fetchDeploy();
  timer = window.setInterval(fetchAll, 15000);
});
onUnmounted(() => window.clearInterval(timer));
</script>

<style scoped>
.sys-cell .kernel {
  font-size: 12px;
  color: var(--td-text-color-secondary);
}
.cmd-box {
  display: flex;
  align-items: center;
  gap: 12px;
  background: #1e1e1e;
  color: #4ec9b0;
  padding: 12px 16px;
  border-radius: 6px;
  font-family: monospace;
  font-size: 13px;
  word-break: break-all;
  margin-bottom: 12px;
}
.cmd-hint {
  font-size: 12px;
  color: var(--td-text-color-secondary);
  line-height: 1.8;
}
</style>
