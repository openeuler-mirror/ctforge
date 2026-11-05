<template>
  <div>
    <!-- 渠道配置 -->
    <t-card title="通知渠道" :bordered="false">
      <template #actions>
        <t-button theme="primary" @click="openChannel()">新增渠道</t-button>
      </template>
      <t-table :data="channels" :columns="chColumns" row-key="id" hover size="small">
        <template #type="{ row }">
          <t-tag variant="outline" size="small">{{ row.type === 'email' ? '邮件' : 'Webhook' }}</t-tag>
        </template>
        <template #target="{ row }">
          <span v-if="row.type === 'email'" class="mono">{{ row.smtp_host }}:{{ row.smtp_port }} → {{ (row.to || []).join(',') }}</span>
          <span v-else class="mono">{{ row.format === 'wecom' ? '企业微信' : '自定义' }} {{ row.url }}</span>
        </template>
        <template #enabled="{ row }">
          <t-switch :model-value="row.enabled" @change="(v: any) => toggleChannel(row, !!v)" />
        </template>
        <template #op="{ row }">
          <t-space break-line>
            <t-link theme="primary" hover="color" @click="onTest(row)">测试</t-link>
            <t-link theme="primary" hover="color" @click="openChannel(row)">编辑</t-link>
            <t-link theme="danger" hover="color" @click="onDelChannel(row)">删除</t-link>
          </t-space>
        </template>
      </t-table>
    </t-card>

    <!-- 告警规则 -->
    <t-card title="告警规则" :bordered="false" style="margin-top: 16px">
      <template #actions>
        <t-button theme="primary" @click="openRule()">新增规则</t-button>
      </template>
      <t-table :data="rulesList" :columns="ruleColumns" row-key="id" hover size="small">
        <template #kinds="{ row }">
          <t-tag v-for="k in row.kinds" :key="k" variant="outline" size="small" style="margin-right: 4px">{{ kindText(k) }}</t-tag>
        </template>
        <template #level="{ row }">{{ levelText(row.min_level) }}及以上</template>
        <template #window="{ row }">
          {{ row.time_windows && row.time_windows.length ? row.time_windows.join(', ') : '全天' }}
        </template>
        <template #enabled="{ row }">
          <t-switch :model-value="row.enabled" @change="(v: any) => toggleRule(row, !!v)" />
        </template>
        <template #op="{ row }">
          <t-space break-line>
            <t-link theme="primary" hover="color" @click="openRule(row)">编辑</t-link>
            <t-link theme="danger" hover="color" @click="onDelRule(row)">删除</t-link>
          </t-space>
        </template>
      </t-table>
    </t-card>

    <!-- 渠道编辑对话框 -->
    <t-dialog v-model:visible="showChannel" :header="editingChannel?.id ? '编辑渠道' : '新增渠道'" width="560px" :on-confirm="saveChannel" :confirm-btn="{ loading: saving }">
      <t-form :data="channelForm" label-width="100px">
        <t-form-item label="名称"><t-input v-model="channelForm.name" /></t-form-item>
        <t-form-item label="类型">
          <t-radio-group v-model="channelForm.type">
            <t-radio value="email">邮件 (SMTP)</t-radio>
            <t-radio value="webhook">Webhook</t-radio>
          </t-radio-group>
        </t-form-item>
        <template v-if="channelForm.type === 'email'">
          <t-form-item label="SMTP 主机"><t-input v-model="channelForm.smtp_host" placeholder="smtp.example.com" /></t-form-item>
          <t-form-item label="端口"><t-input-number v-model="channelForm.smtp_port" :min="1" :max="65535" placeholder="587" /></t-form-item>
          <t-form-item label="用户名"><t-input v-model="channelForm.username" /></t-form-item>
          <t-form-item label="密码"><t-input v-model="channelForm.password" type="password" /></t-form-item>
          <t-form-item label="发件人"><t-input v-model="channelForm.from" placeholder="alerts@example.com" /></t-form-item>
          <t-form-item label="收件人"><t-input v-model="channelForm.toText" placeholder="多个用英文逗号分隔" /></t-form-item>
          <t-form-item label="TLS"><t-switch v-model="channelForm.use_tls" /></t-form-item>
        </template>
        <template v-else>
          <t-form-item label="Webhook URL"><t-input v-model="channelForm.url" placeholder="https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=xxx" /></t-form-item>
          <t-form-item label="格式">
            <t-select v-model="channelForm.format">
              <t-option value="wecom" label="企业微信机器人（markdown）" />
              <t-option value="json" label="自定义 JSON" />
            </t-select>
          </t-form-item>
        </template>
        <t-form-item label="启用"><t-switch v-model="channelForm.enabled" /></t-form-item>
      </t-form>
    </t-dialog>

    <!-- 规则编辑对话框 -->
    <t-dialog v-model:visible="showRule" :header="editingRule?.id ? '编辑规则' : '新增规则'" width="560px" :on-confirm="saveRule" :confirm-btn="{ loading: saving }">
      <t-form :data="ruleForm" label-width="100px">
        <t-form-item label="名称"><t-input v-model="ruleForm.name" placeholder="如：离线告警" /></t-form-item>
        <t-form-item label="事件类型">
          <t-checkbox-group v-model="ruleForm.kinds">
            <t-checkbox value="*">全部</t-checkbox>
            <t-checkbox value="offline">客户端离线</t-checkbox>
            <t-checkbox value="high_cpu">CPU 过高</t-checkbox>
            <t-checkbox value="high_mem">内存过高</t-checkbox>
            <t-checkbox value="high_disk">磁盘过高</t-checkbox>
            <t-checkbox value="net_spike">流量异常</t-checkbox>
            <t-checkbox value="ssh_brute">SSH 爆破</t-checkbox>
          </t-checkbox-group>
        </t-form-item>
        <t-form-item label="最低级别">
          <t-select v-model="ruleForm.min_level">
            <t-option value="info" label="信息" />
            <t-option value="warning" label="警告" />
            <t-option value="critical" label="严重" />
          </t-select>
        </t-form-item>
        <t-form-item label="发送渠道">
          <t-checkbox-group v-model="ruleForm.channels">
            <t-checkbox v-for="c in channels" :key="c.id" :value="c.id">{{ c.name }}</t-checkbox>
          </t-checkbox-group>
        </t-form-item>
        <t-form-item label="时间段">
          <t-input v-model="ruleForm.windowText" placeholder="如 09:00-22:00，留空=全天；多段用逗号分隔" />
        </t-form-item>
        <t-form-item label="静默期（秒）"><t-input-number v-model="ruleForm.cooldown_sec" :min="0" :max="86400" /></t-form-item>
        <t-form-item label="启用"><t-switch v-model="ruleForm.enabled" /></t-form-item>
      </t-form>
    </t-dialog>
  </div>
</template>

<script setup lang="ts">
import { onMounted, ref } from 'vue';
import { MessagePlugin, DialogPlugin } from 'tdesign-vue-next';
import type { PrimaryTableCol } from 'tdesign-vue-next';

import {
  getNotifyConfig, saveNotifyConfig, deleteChannel, deleteRule as apiDeleteRule,
  testChannel, upsertChannel, upsertRule,
  type NotifyChannel, type NotifyRule,
} from '@/api/agent';

const channels = ref<NotifyChannel[]>([]);
const rulesList = ref<NotifyRule[]>([]);
const saving = ref(false);
const showChannel = ref(false);
const showRule = ref(false);
const editingChannel = ref<NotifyChannel | null>(null);
const editingRule = ref<NotifyRule | null>(null);

const emptyChannel = () => ({
  name: '', type: 'webhook' as const, enabled: true,
  smtp_host: '', smtp_port: 587, username: '', password: '', from: '', toText: '', use_tls: false,
  url: '', format: 'wecom',
});
const emptyRule = () => ({
  name: '', enabled: true, kinds: ['*'] as string[], min_level: 'warning',
  channels: [] as string[], windowText: '', cooldown_sec: 300,
});

const channelForm = ref<any>(emptyChannel());
const ruleForm = ref<any>(emptyRule());

const chColumns: PrimaryTableCol[] = [
  { colKey: 'name', title: '名称', width: 140 },
  { colKey: 'type', title: '类型', width: 100 },
  { colKey: 'target', title: '目标', ellipsis: true },
  { colKey: 'enabled', title: '启用', width: 70 },
  { colKey: 'op', title: '操作', width: 140, fixed: 'right' },
];

const ruleColumns: PrimaryTableCol[] = [
  { colKey: 'name', title: '规则名', width: 140 },
  { colKey: 'kinds', title: '事件', width: 220 },
  { colKey: 'level', title: '级别', width: 100 },
  { colKey: 'window', title: '时间段', width: 160 },
  { colKey: 'enabled', title: '启用', width: 70 },
  { colKey: 'op', title: '操作', width: 120, fixed: 'right' },
];

function kindText(k: string) {
  return { '*': '全部', offline: '离线', high_cpu: 'CPU', high_mem: '内存', high_disk: '磁盘', net_spike: '流量', ssh_brute: 'SSH 爆破' }[k] || k;
}
function levelText(l: string) {
  return { info: '信息', warning: '警告', critical: '严重' }[l] || l;
}

async function fetchAll() {
  try {
    const cfg = await getNotifyConfig();
    channels.value = cfg.channels || [];
    rulesList.value = cfg.rules || [];
  } catch (e: any) {
    MessagePlugin.error(e.message || '加载失败');
  }
}

function openChannel(ch?: NotifyChannel) {
  editingChannel.value = ch || null;
  channelForm.value = ch ? { ...ch, toText: (ch.to || []).join(',') } : emptyChannel();
  showChannel.value = true;
}

async function saveChannel() {
  saving.value = true;
  try {
    const f = channelForm.value;
    const payload: any = {
      name: f.name, type: f.type, enabled: f.enabled,
    };
    if (f.type === 'email') {
      Object.assign(payload, {
        smtp_host: f.smtp_host, smtp_port: f.smtp_port, username: f.username,
        password: f.password, from: f.from, use_tls: f.use_tls,
        to: (f.toText || '').split(',').map((s: string) => s.trim()).filter(Boolean),
      });
    } else {
      Object.assign(payload, { url: f.url, format: f.format });
    }
    if (editingChannel.value?.id) payload.id = editingChannel.value.id;
    await upsertChannel(payload);
    MessagePlugin.success('已保存');
    showChannel.value = false;
    fetchAll();
  } catch (e: any) {
    MessagePlugin.error(e.message || '保存失败');
  } finally {
    saving.value = false;
  }
}

async function toggleChannel(ch: NotifyChannel, v: boolean) {
  try {
    await upsertChannel({ ...ch, enabled: v });
    ch.enabled = v;
  } catch (e: any) {
    MessagePlugin.error(e.message || '操作失败');
  }
}

function onDelChannel(ch: NotifyChannel) {
  const dlg = DialogPlugin.confirm({
    header: '删除渠道', body: `确定删除渠道"${ch.name}"吗？`, theme: 'danger',
    onConfirm: async () => {
      try {
        await deleteChannel(ch.id);
        MessagePlugin.success('已删除');
        fetchAll();
      } catch (e: any) {
        MessagePlugin.error(e.message || '删除失败');
      }
      dlg.destroy();
    },
  });
}

async function onTest(ch: NotifyChannel) {
  try {
    await testChannel(ch.id);
    MessagePlugin.success('测试消息已发送');
  } catch (e: any) {
    MessagePlugin.error(e.message || '发送失败');
  }
}

function openRule(r?: NotifyRule) {
  editingRule.value = r || null;
  ruleForm.value = r ? { ...r, windowText: (r.time_windows || []).join(',') } : emptyRule();
  showRule.value = true;
}

async function saveRule() {
  saving.value = true;
  try {
    const f = ruleForm.value;
    const payload: any = {
      name: f.name, enabled: f.enabled, kinds: f.kinds, min_level: f.min_level,
      channels: f.channels, cooldown_sec: f.cooldown_sec,
      time_windows: (f.windowText || '').split(',').map((s: string) => s.trim()).filter(Boolean),
    };
    if (editingRule.value?.id) payload.id = editingRule.value.id;
    if (!payload.id) payload.id = `rule-${Date.now()}`;
    await upsertRule(payload);
    MessagePlugin.success('已保存');
    showRule.value = false;
    fetchAll();
  } catch (e: any) {
    MessagePlugin.error(e.message || '保存失败');
  } finally {
    saving.value = false;
  }
}

async function toggleRule(r: NotifyRule, v: boolean) {
  try {
    await upsertRule({ ...r, enabled: v });
    r.enabled = v;
  } catch (e: any) {
    MessagePlugin.error(e.message || '操作失败');
  }
}

function onDelRule(r: NotifyRule) {
  const dlg = DialogPlugin.confirm({
    header: '删除规则', body: `确定删除规则"${r.name}"吗？`, theme: 'danger',
    onConfirm: async () => {
      try {
        await apiDeleteRule(r.id);
        MessagePlugin.success('已删除');
        fetchAll();
      } catch (e: any) {
        MessagePlugin.error(e.message || '删除失败');
      }
      dlg.destroy();
    },
  });
}

onMounted(fetchAll);
</script>

<style scoped>
.mono { font-family: monospace; font-size: 12px; word-break: break-all; }
</style>
