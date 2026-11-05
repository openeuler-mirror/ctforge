<template>
  <div>
    <!-- 权限元信息说明 -->
    <t-card :bordered="false" style="margin-bottom: 16px">
      <template #title>权限等级说明</template>
      <t-row :gutter="12">
        <t-col :span="6" v-for="g in permGroups" :key="g.id">
          <t-card :bordered="false" class="perm-group-card" :class="`level-${g.level}`">
            <div class="group-head">
              <span class="level-badge">L{{ g.level }}</span>
              <span class="group-name">{{ g.name }}</span>
            </div>
            <div class="group-desc">{{ levelDesc(g.level) }}</div>
          </t-card>
        </t-col>
      </t-row>
    </t-card>

    <!-- 分组管理 -->
    <t-card title="主机分组" :bordered="false">
      <template #actions>
        <t-button theme="primary" @click="openGroup()">新建分组</t-button>
      </template>
      <t-table :data="groups" :columns="groupColumns" row-key="id" hover size="small">
        <template #agents="{ row }">
          <t-tag v-for="a in row.agents.slice(0, 3)" :key="a" variant="outline" size="small" style="margin-right: 4px">{{ a }}</t-tag>
          <span v-if="row.agents.length > 3" class="more">+{{ row.agents.length - 3 }}</span>
        </template>
        <template #op="{ row }">
          <t-space break-line>
            <t-link theme="primary" hover="color" @click="openGroup(row)">编辑</t-link>
            <t-link theme="danger" hover="color" @click="onDelGroup(row)">删除</t-link>
          </t-space>
        </template>
      </t-table>
    </t-card>

    <!-- 账户授权 -->
    <t-card title="账户授权" :bordered="false" style="margin-top: 16px">
      <template #actions>
        <t-button theme="primary" @click="openGrant()">新增授权</t-button>
      </template>
      <t-table :data="grants" :columns="grantColumns" row-key="username" hover size="small">
        <template #perms="{ row }">
          <t-tag v-if="row.permissions.includes('system.all')" theme="danger" variant="light" size="small">全部权限</t-tag>
          <template v-else>
            <t-tag v-for="p in row.permissions.slice(0, 3)" :key="p" variant="outline" size="small" style="margin-right: 4px">{{ permText(p) }}</t-tag>
            <span v-if="row.permissions.length > 3" class="more">+{{ row.permissions.length - 3 }}</span>
          </template>
        </template>
        <template #scope="{ row }">
          <span v-if="row.scope.all">全部主机</span>
          <span v-else>{{ (row.scope.groups || []).length }} 组 + {{ (row.scope.agents || []).length }} 台</span>
        </template>
        <template #expires="{ row }">
          <span v-if="!row.expires_at">永不过期</span>
          <span v-else :class="{ expired: row.expires_at < now }">{{ formatMs(row.expires_at) }}</span>
        </template>
        <template #op="{ row }">
          <t-space break-line>
            <t-link theme="primary" hover="color" @click="openGrant(row)">编辑</t-link>
            <t-link theme="danger" hover="color" @click="onDelGrant(row)">删除</t-link>
          </t-space>
        </template>
      </t-table>
    </t-card>

    <!-- 分组编辑对话框 -->
    <t-dialog v-model:visible="showGroup" :header="editingGroup?.id ? '编辑分组' : '新建分组'" width="640px" :on-confirm="saveGroup" :confirm-btn="{ loading: saving }">
      <t-form :data="groupForm" label-width="90px">
        <t-form-item label="名称"><t-input v-model="groupForm.name" placeholder="如：生产环境 / 测试环境" /></t-form-item>
        <t-form-item label="描述"><t-input v-model="groupForm.description" placeholder="可选" /></t-form-item>
        <t-form-item label="包含主机">
          <t-transfer
            v-model="groupForm.agents"
            :data="agentTransferData"
            @change="(v: any) => groupForm.agents = v as string[]"
          />
        </t-form-item>
      </t-form>
    </t-dialog>

    <!-- 授权编辑对话框 -->
    <t-dialog v-model:visible="showGrant" :header="`授权：${editingGrant?.username ?? ''}`" width="720px" :on-confirm="saveGrantFn" :confirm-btn="{ loading: saving }">
      <t-form :data="grantForm" label-width="100px">
        <t-form-item label="账户">
          <t-select v-model="grantForm.username" :disabled="!!editingGrant?.username" :options="accountOptions" />
        </t-form-item>
        <t-form-item label="权限点">
          <div class="perm-picker">
            <t-checkbox :model-value="grantForm.permissions.includes('system.all')" @change="onToggleAll">全部权限（超管）</t-checkbox>
            <div class="perm-grid">
              <t-checkbox
                v-for="p in allPerms.filter(x => x !== 'system.all')"
                :key="p"
                :model-value="grantForm.permissions.includes(p)"
                @change="(v: any) => onTogglePerm(p, !!v)"
              >
                {{ permText(p) }}
              </t-checkbox>
            </div>
          </div>
        </t-form-item>
        <t-form-item label="机器范围">
          <t-radio-group v-model="scopeMode">
            <t-radio value="all">全部主机</t-radio>
            <t-radio value="partial">指定范围</t-radio>
          </t-radio-group>
          <div v-if="scopeMode === 'partial'" class="scope-picker">
            <div>
              <div class="scope-label">允许的分组：</div>
              <t-checkbox-group v-model="grantForm.scopeGroups">
                <t-checkbox v-for="g in groups" :key="g.id" :value="g.id">{{ g.name }}</t-checkbox>
              </t-checkbox-group>
            </div>
            <div>
              <div class="scope-label">额外指定的主机：</div>
              <t-checkbox-group v-model="grantForm.scopeAgents">
                <t-checkbox v-for="a in agentList" :key="a.agent_id" :value="a.agent_id">{{ a.hostname }}</t-checkbox>
              </t-checkbox-group>
            </div>
          </div>
        </t-form-item>
        <t-form-item label="有效期">
          <t-date-picker v-model="expiryDate" placeholder="留空表示永不过期" enable-time-picker clearable />
        </t-form-item>
      </t-form>
    </t-dialog>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { MessagePlugin, DialogPlugin } from 'tdesign-vue-next';
import type { PrimaryTableCol } from 'tdesign-vue-next';
import dayjs from 'dayjs';

import {
  createGroup, deleteGroup, deleteGrant, getGrant, getPermsMeta, listAccounts,
  listAgents, listGroups, listGrants, saveGrant as apiSaveGrant, updateGroup,
  type Agent, type AgentGroup, type Grant, type PermGroupDef,
} from '@/api/agent';

const permGroups = ref<PermGroupDef[]>([]);
const allPerms = ref<string[]>([]);
const groups = ref<AgentGroup[]>([]);
const grants = ref<Grant[]>([]);
const accounts = ref<any[]>([]);
const agentList = ref<Agent[]>([]);
const saving = ref(false);
const showGroup = ref(false);
const showGrant = ref(false);
const editingGroup = ref<AgentGroup | null>(null);
const editingGrant = ref<Grant | null>(null);
const scopeMode = ref<'all' | 'partial'>('all');
const expiryDate = ref<any>(null);
const now = Date.now();

const groupForm = ref({ name: '', description: '', agents: [] as string[] });
const grantForm = ref({
  username: '',
  permissions: [] as string[],
  scopeGroups: [] as string[],
  scopeAgents: [] as string[],
});

const groupColumns: PrimaryTableCol[] = [
  { colKey: 'name', title: '分组名', width: 160 },
  { colKey: 'description', title: '描述', ellipsis: true },
  { colKey: 'agents', title: '主机', ellipsis: true },
  { colKey: 'op', title: '操作', width: 120, fixed: 'right' },
];

const grantColumns: PrimaryTableCol[] = [
  { colKey: 'username', title: '账户', width: 140 },
  { colKey: 'perms', title: '权限', ellipsis: true },
  { colKey: 'scope', title: '机器范围', width: 160 },
  { colKey: 'expires', title: '有效期至', width: 180 },
  { colKey: 'op', title: '操作', width: 120, fixed: 'right' },
];

const agentTransferData = computed(() =>
  agentList.value.map((a) => ({ label: `${a.hostname} (${a.ip})`, value: a.agent_id })),
);

const accountOptions = computed(() =>
  accounts.value.map((a) => ({ label: `${a.username}（${a.display_name || a.role}）`, value: a.username })),
);

function levelDesc(level: number) {
  return { 1: '查看监控/日志/审计，不可操作', 2: '可执行日常运维操作', 3: '可执行高危操作（文件编辑/升级/卸载）', 4: '系统管理全部权限' }[level] || '';
}

function permText(p: string) {
  const m: Record<string, string> = {
    'agent.view': '查看主机', 'agent.metrics': '查看指标', 'agent.logs': '查看日志',
    'agent.terminal': '远程终端', 'agent.file.view': '查看文件', 'agent.file.edit': '编辑文件',
    'agent.cmd.exec': '执行命令', 'agent.service': '服务管理', 'agent.pkg': '软件包',
    'agent.sysuser': '系统账户', 'agent.syslog': '系统日志', 'agent.security': '安全审计',
    'agent.portfwd': '端口转发', 'agent.upgrade': '远程升级', 'agent.uninstall': '远程卸载',
    'system.deploy': '部署客户端', 'system.approval': '批准管理', 'system.task': '定时任务',
    'system.alert': '告警管理', 'system.notify': '通知配置', 'system.accounts': '账户管理',
    'system.audit': '审计日志', 'system.stats': '统计视图', 'system.topology': '拓扑视图',
    'system.ai': 'AI 分析', 'system.group': '分组管理', 'system.all': '全部权限',
  };
  return m[p] || p;
}

function formatMs(ms?: number) {
  return ms ? dayjs(ms).format('YYYY-MM-DD HH:mm') : '-';
}

async function fetchAll() {
  const [meta, gs, grs, accs, agents] = await Promise.all([
    getPermsMeta(), listGroups(), listGrants(), listAccounts(), listAgents(),
  ]);
  permGroups.value = meta.groups;
  allPerms.value = meta.perms;
  groups.value = gs || [];
  grants.value = grs || [];
  accounts.value = accs || [];
  agentList.value = agents.filter((a) => a.approval === 'approved');
}

function openGroup(g?: AgentGroup) {
  editingGroup.value = g || null;
  groupForm.value = g
    ? { name: g.name, description: g.description || '', agents: [...(g.agents || [])] }
    : { name: '', description: '', agents: [] };
  showGroup.value = true;
}

async function saveGroup() {
  if (!groupForm.value.name) { MessagePlugin.warning('请填写分组名'); return; }
  saving.value = true;
  try {
    if (editingGroup.value?.id) await updateGroup(editingGroup.value.id, groupForm.value);
    else await createGroup(groupForm.value);
    MessagePlugin.success('已保存');
    showGroup.value = false;
    fetchAll();
  } catch (e: any) {
    MessagePlugin.error(e.message || '保存失败');
  } finally {
    saving.value = false;
  }
}

function onDelGroup(g: AgentGroup) {
  const dlg = DialogPlugin.confirm({
    header: '删除分组', body: `确定删除分组"${g.name}"吗？`, theme: 'danger',
    onConfirm: async () => {
      try {
        await deleteGroup(g.id);
        MessagePlugin.success('已删除');
        fetchAll();
      } catch (e: any) {
        MessagePlugin.error(e.message || '删除失败');
      }
      dlg.destroy();
    },
  });
}

async function openGrant(g?: Grant) {
  if (g) {
    editingGrant.value = g;
    grantForm.value = {
      username: g.username,
      permissions: [...g.permissions],
      scopeGroups: [...(g.scope.groups || [])],
      scopeAgents: [...(g.scope.agents || [])],
    };
    scopeMode.value = g.scope.all ? 'all' : 'partial';
    expiryDate.value = g.expires_at ? dayjs(g.expires_at).toDate() : null;
  } else {
    editingGrant.value = null;
    grantForm.value = { username: '', permissions: [], scopeGroups: [], scopeAgents: [] };
    scopeMode.value = 'all';
    expiryDate.value = null;
  }
  showGrant.value = true;
}

function onToggleAll(v: any) {
  if (v) grantForm.value.permissions = ['system.all'];
  else grantForm.value.permissions = [];
}

function onTogglePerm(p: string, on: boolean) {
  const arr = grantForm.value.permissions;
  if (on && !arr.includes(p)) arr.push(p);
  if (!on) grantForm.value.permissions = arr.filter((x) => x !== p);
}

async function saveGrantFn() {
  if (!grantForm.value.username) { MessagePlugin.warning('请选择账户'); return; }
  saving.value = true;
  try {
    const payload: any = {
      username: grantForm.value.username,
      permissions: grantForm.value.permissions,
      scope: scopeMode.value === 'all'
        ? { all: true }
        : { all: false, groups: grantForm.value.scopeGroups, agents: grantForm.value.scopeAgents },
      expires_at: expiryDate.value ? dayjs(expiryDate.value as Date).valueOf() : 0,
    };
    await apiSaveGrant(grantForm.value.username, payload);
    MessagePlugin.success('已保存');
    showGrant.value = false;
    fetchAll();
  } catch (e: any) {
    MessagePlugin.error(e.message || '保存失败');
  } finally {
    saving.value = false;
  }
}

function onDelGrant(g: Grant) {
  const dlg = DialogPlugin.confirm({
    header: '删除授权', body: `确定删除 ${g.username} 的全部授权吗？`, theme: 'danger',
    onConfirm: async () => {
      try {
        await deleteGrant(g.username);
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
.perm-group-card { border-left: 4px solid var(--td-brand-color); }
.level-1 { border-color: #0594fa; }
.level-2 { border-color: #00a870; }
.level-3 { border-color: #ed7b2f; }
.level-4 { border-color: #e34d59; }
.group-head { display: flex; align-items: center; gap: 8px; margin-bottom: 8px; }
.level-badge { font-size: 11px; background: var(--td-bg-color-container-hover); padding: 2px 6px; border-radius: 3px; font-weight: 600; }
.group-name { font-weight: 600; }
.group-desc { font-size: 12px; color: var(--td-text-color-secondary); }
.more { font-size: 12px; color: var(--td-text-color-secondary); }
.expired { color: #e34d59; }
.perm-picker { width: 100%; }
.perm-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; margin-top: 8px; }
.scope-picker { margin-top: 12px; padding: 12px; background: var(--td-bg-color-container-hover); border-radius: 6px; }
.scope-label { font-size: 12px; font-weight: 600; margin-bottom: 8px; }
</style>
