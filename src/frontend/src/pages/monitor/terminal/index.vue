<template>
  <div class="term-page">
    <!-- 顶部工具栏 -->
    <div class="term-toolbar">
      <t-button theme="primary" size="small" @click="addTab">+ 新建标签</t-button>
      <t-divider layout="vertical" />
      <span class="hint">分屏：</span>
      <t-button-group size="small" variant="outline">
        <t-button :theme="layout === '1' ? 'primary' : 'default'" @click="setLayout('1')">单屏</t-button>
        <t-button :theme="layout === '2v' ? 'primary' : 'default'" @click="setLayout('2v')">左右</t-button>
        <t-button :theme="layout === '2h' ? 'primary' : 'default'" @click="setLayout('2h')">上下</t-button>
        <t-button :theme="layout === '4' ? 'primary' : 'default'" @click="setLayout('4')">四分</t-button>
      </t-button-group>
      <t-divider layout="vertical" />
      <t-button size="small" theme="default" @click="showSuspended = true">
        挂起会话（{{ suspendedCount }}）
      </t-button>
      <t-divider layout="vertical" />
      <t-dropdown trigger="click" placement="bottom">
        <t-button size="small" theme="default">风格</t-button>
        <template #dropdown>
          <t-dropdown-item v-for="(cfg, key) in TERM_STYLES" :key="key" @click="setTermStyle(key)">
            <t-icon v-if="key === termStyle" name="check" size="14" style="margin-right:6px" />
            <span v-else style="display:inline-block;width:20px"></span>
            {{ cfg.name }}
          </t-dropdown-item>
        </template>
      </t-dropdown>
      <t-divider layout="vertical" />
      <t-button size="small" theme="default" @click="toggleFullscreen">
        {{ isFullscreen ? '退出全屏' : '全屏' }}
      </t-button>
      <t-divider layout="vertical" />
      <t-button size="small" theme="default" @click="showHistory = !showHistory">
        命令历史
      </t-button>
      <t-button
        size="small"
        :theme="showMonitor ? 'primary' : 'default'"
        @click="toggleMonitor"
      >资源监控</t-button>
      <t-button
        size="small"
        :theme="showTransfer ? 'primary' : 'default'"
        @click="toggleTransfer"
      >文件传输</t-button>
      <t-tooltip content="快捷键帮助">
        <t-button theme="default" size="small" variant="text" @click="showShortcuts = true">
          <t-icon name="help-circle" />
        </t-button>
      </t-tooltip>
    </div>

    <!-- 标签栏 -->
    <div class="tab-bar">
      <div
        v-for="tab in tabs"
        :key="tab.id"
        class="tab-item"
        :class="{ active: tab.id === activeTabId }"
        @click="activeTabId = tab.id; nextTick(() => { refreshSidePanels(); rebuildActiveTerminals(); })"
      >
        <span
          class="tab-title"
          :title="'双击改名'"
          @dblclick.stop="renameTab(tab)"
        >{{ tab.customTitle || tab.title }}</span>
        <span
          class="tab-close"
          title="关闭标签"
          role="button"
          aria-label="关闭标签"
          @click.stop="confirmCloseTab(tab.id)"
          @mousedown.stop
        >&times;</span>
      </div>
    </div>

    <!-- 工作区：单窗口时终端 + 右侧栏 + 下方面板内嵌并排 -->
    <div class="work-area" :class="{ 'inline-panels': isSinglePane }">
      <!-- 终端网格 -->
      <div ref="gridEl" class="term-grid" :class="`layout-${layout}`">
      <!-- 所有 pane 常驻渲染，v-show 按 tab 显示/隐藏（切 tab 不销毁 DOM，xterm 文本保留） -->
      <div
        v-for="(pane, idx) in allPanes"
        :key="pane.id"
        v-show="activeTab?.panes.includes(pane.id)"
        class="pane"
        :class="{ focused: pane.id === focusedPaneId }"
        :style="paneStyle(idx)"
      >
        <div class="pane-bar">
          <t-select
            :model-value="paneSelectValue(pane)"
            :options="paneSelectOptions"
            placeholder="主机或挂起会话"
            size="small"
            style="width: 300px"
            @change="(v: any) => onPaneTargetChange(pane, String(v))"
          >
            <template #valueDisplay="{ value }">
              <span class="pane-title" :title="'双击改备注名'" @dblclick.stop="renamePane(pane)">{{ paneTitle(pane) }}</span>
            </template>
          </t-select>
          <t-space :size="4">
            <t-checkbox
              :model-value="pane.suspend"
              size="small"
              class="suspend-white"
              @change="(v: any) => toggleSuspend(pane, !!v)"
            >挂起</t-checkbox>
            <span class="pane-state" :class="`st-${pane.state}`">
              {{ pane.state === 'open' ? '●' : pane.state === 'connecting' ? '○…' : '○' }}
            </span>
            <t-icon name="edit-1" class="pane-ico" :title="'改备注名'" @click="renamePane(pane)" />
            <t-icon name="refresh" class="pane-ico" @click="reconnectPane(pane)" />
            <t-icon name="fullscreen" class="pane-ico" @click="fullscreenPane(pane)" />
          </t-space>
        </div>
        <div :ref="(el) => setTermEl(pane.id, el as HTMLElement)" class="pane-body" @click="focusPane(pane)"></div>
      </div>
      </div>

      <!-- 内嵌右侧栏（单窗口：文件传输） -->
      <!-- 文件传输侧栏 -->
    <div v-if="showTransfer" class="tsp transfer-panel inline-bottom">
      <div class="side-head">
        <span>文件传输 · {{ transferPath }}</span>
        <t-tooltip content="同步终端工作目录">
          <t-icon name="refresh" style="cursor:pointer;margin-right:8px" @click="syncCwd" />
        </t-tooltip>
        <t-icon name="close" @click="showTransfer = false" />
      </div>
      <div class="side-toolbar">
        <t-button size="small" variant="outline" @click="tGoUp" :disabled="transferPath === '/'">上级</t-button>
        <t-button size="small" theme="primary" @click="tUpload">上传</t-button>
        <t-button-group size="small" variant="outline">
          <t-button :theme="transferView === 'list' ? 'primary' : 'default'" @click="setTransferView('list')">
            <t-icon name="view-list" />
          </t-button>
          <t-button :theme="transferView === 'icon' ? 'primary' : 'default'" @click="setTransferView('icon')">
            <t-icon name="view-module" />
          </t-button>
        </t-button-group>
        <input ref="tFileInput" type="file" style="display:none" @change="tOnPick" multiple />
      </div>
      <t-progress v-if="transferUploading" :percentage="uploadProgress" size="small" style="padding: 0 10px" />
      <div class="side-body" :class="{ 'icon-mode': transferView === 'icon' }">
        <t-loading :loading="transferLoading" size="small">
          <!-- 列表模式 -->
          <template v-if="transferView === 'list'">
            <div
              v-for="f in transferFiles"
              :key="f.path"
              class="tf-item"
              @click="tEnter(f)"
              :title="f.name"
            >
              <t-icon :name="f.is_dir ? 'folder' : tfIcon(f)" :class="f.is_dir ? 'tf-dir' : 'tf-file'" />
              <span class="tf-name">{{ f.name }}</span>
              <span class="tf-size">{{ f.is_dir ? '' : fmtSize(f.size) }}</span>
              <t-icon v-if="!f.is_dir" name="edit" class="tf-dl" @click.stop="tEditFile(f)" />
              <t-icon v-if="!f.is_dir" name="download" class="tf-dl" @click.stop="tDownload(f)" />
            </div>
          </template>
          <!-- 图标模式 -->
          <div v-else class="tf-icon-grid">
            <div
              v-for="f in transferFiles"
              :key="f.path"
              class="tf-icon-item"
              :title="f.name"
              @dblclick="tEnter(f)"
              @click="selectedTfPath = f.path"
              @contextmenu.prevent.stop="onTfCtxMenu($event, f)"
              :class="{ selected: selectedTfPath === f.path }"
            >
              <t-icon :name="f.is_dir ? 'folder' : tfIcon(f)" size="36px"
                      :class="f.is_dir ? 'tf-dir' : 'tf-file'" />
              <div class="tf-icon-name">{{ f.name }}</div>
              <div class="tf-icon-size">{{ f.is_dir ? '' : fmtSize(f.size) }}</div>
            </div>
          </div>
          <div v-if="!transferFiles.length && !transferLoading" class="tf-empty">空目录</div>
        </t-loading>
      </div>
    </div>

    <!-- 文件传输右键菜单（fullscreen 时挂载到 .term-page，避免被全屏层叠上下文遮挡） -->
    <Teleport :to="ctxMenuTarget">
      <div v-if="tfCtxMenu.show" class="tf-ctx-menu" :style="{ left: tfCtxMenu.x + 'px', top: tfCtxMenu.y + 'px' }" @click.stop>
        <template v-if="tfCtxMenu.file">
          <div v-if="!tfCtxMenu.file.is_dir" class="tf-ctx-item" @click="tEditFile(tfCtxMenu.file); hideTfCtxMenu()">
            <t-icon name="edit" /> 编辑
          </div>
          <div v-if="!tfCtxMenu.file.is_dir" class="tf-ctx-item" @click="tDownload(tfCtxMenu.file); hideTfCtxMenu()">
            <t-icon name="download" /> 下载
          </div>
          <div class="tf-ctx-item danger" @click="tDeleteFile(tfCtxMenu.file); hideTfCtxMenu()">
            <t-icon name="delete" /> 删除
          </div>
        </template>
      </div>
    </Teleport>

    <!-- 快捷键帮助对话框 -->
    <t-dialog v-model:visible="showShortcuts" header="终端快捷键" width="520px" :footer="false">
      <div class="shortcut-list">
        <div class="shortcut-row"><span class="keys"><kbd>Ctrl</kbd>+<kbd>`</kbd></span><span>切换到下一个窗口</span></div>
        <div class="shortcut-row"><span class="keys"><kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>`</kbd></span><span>切换到上一个窗口</span></div>
        <div class="shortcut-row"><span class="keys"><kbd>Ctrl</kbd>+<kbd>1</kbd>~<kbd>9</kbd></span><span>直接定位到第 N 个窗口</span></div>
        <div class="shortcut-row"><span class="keys"><kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>↑</kbd><kbd>↓</kbd><kbd>←</kbd><kbd>→</kbd></span><span>按方向切换（四分屏上下左右均可）</span></div>
        <div class="shortcut-row"><span class="keys"><kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>F</kbd></span><span>当前窗口全屏 / 还原</span></div>
        <div class="shortcut-row"><span class="keys"><kbd>Ctrl</kbd>+<kbd>V</kbd></span><span>粘贴剪贴板内容</span></div>
        <div class="shortcut-row"><span class="keys"><kbd>右键</kbd></span><span>选中复制 / 无选中粘贴</span></div>
        <div class="shortcut-hint">文件管理 / 资源监控自动跟随当前聚焦窗口的路径与主机</div>
      </div>
    </t-dialog>

    <!-- 文件编辑对话框 -->
    <t-dialog v-model:visible="tShowEditor" :header="`编辑：${tEditPath}`" width="900px" :footer="false">
      <div v-if="tEditBinary" class="editor-binary">二进制文件无法在线编辑，请下载后修改。</div>
      <template v-else>
        <div class="editor-toolbar">
          <span class="editor-info">{{ tEditPath }}</span>
          <t-input v-model="tSearchQuery" placeholder="查找" size="small" clearable style="width:160px" @enter="tFindNext" @change="tFindNext" />
          <t-button size="small" variant="outline" @click="tFindNext">查找</t-button>
          <t-checkbox v-model="tEditMarkdown" size="small" @change="tInitEditor">Markdown</t-checkbox>
          <t-radio-group v-model="tEditMode" size="small" @change="tInitEditor">
            <t-radio-button value="view">查看</t-radio-button>
            <t-radio-button value="edit">编辑</t-radio-button>
          </t-radio-group>
          <t-button size="small" theme="primary" :loading="tEditSaving" :disabled="tEditBinary || tEditTruncated" @click="tSaveFile">保存</t-button>
        </div>
        <div ref="tEditorEl" class="editor-wrap"></div>
        <div v-if="tEditTruncated" class="editor-truncated">文件过大已截断显示，保存将覆盖完整文件。</div>
      </template>
    </t-dialog>

    <!-- 内嵌下方面板（单窗口：资源监控） -->
    <!-- 资源监控侧栏（动态仪表盘） -->
    <div v-if="showMonitor" class="tsp monitor-panel inline-right">
      <div class="side-head">
        <span>资源监控 · {{ currentPaneAgent || '未连接' }}</span>
        <t-icon name="close" @click="showMonitor = false" />
      </div>
      <div class="side-body dash-body">
        <t-loading :loading="monLoading" size="small">
          <template v-if="monData">
            <!-- 系统信息 -->
            <div class="dash-section sys-info">
              <div class="sys-hostname">{{ monData.hostname || currentPaneAgent }}</div>
              <div class="sys-meta">
                <span class="sys-tag">{{ monData.os || '-' }}</span>
                <span class="sys-tag ip">{{ monData.ip || '-' }}</span>
              </div>
            </div>

            <!-- CPU 占用 -->
            <div class="dash-section">
              <div class="dash-title">CPU <span class="dash-val">{{ pct(monData.cpu_percent) }}%</span></div>
              <div class="bar-wrap">
                <div class="bar-fill" :style="{ width: Math.min(100, monData.cpu_percent || 0) + '%', background: barColor(monData.cpu_percent || 0) }"></div>
              </div>
              <div class="mon-sub" v-if="monData.cpu_count">共 {{ monData.cpu_count }} 核 · 负载 {{ num(monData.load1) }}/{{ num(monData.load5) }}/{{ num(monData.load15) }}</div>
            </div>

            <!-- 内存 -->
            <div class="dash-section">
              <div class="dash-title">内存 <span class="dash-val">{{ pct(monData.mem_percent) }}%</span></div>
              <div class="bar-wrap mem">
                <div class="bar-fill used" :style="{ width: memUsedPct + '%' }"></div>
                <div class="bar-fill cached" :style="{ width: memCachedPct + '%', left: memUsedPct + '%' }" v-if="memCachedPct > 0"></div>
              </div>
              <div class="mem-legend">
                <span><i class="dot used"></i>已用 {{ fmtKB(monData.mem_used_kb) }}</span>
                <span v-if="monData.mem_cached_kb"><i class="dot cached"></i>缓存 {{ fmtKB(monData.mem_cached_kb) }}</span>
                <span><i class="dot free"></i>空闲 {{ fmtKB((monData.mem_total_kb || 0) - (monData.mem_used_kb || 0)) }}</span>
              </div>
              <div class="mon-sub">总计 {{ fmtKB(monData.mem_total_kb) }}</div>
            </div>

            <!-- 网络：速率 + 流量曲线图 -->
            <div class="dash-section">
              <div class="dash-title">网络</div>
              <div class="net-now">
                <div class="net-cell">
                  <div class="net-label">↓ 下载</div>
                  <div class="net-val rx">{{ fmtRate(monData.net_rx) }}</div>
                </div>
                <div class="net-cell">
                  <div class="net-label">↑ 上传</div>
                  <div class="net-val tx">{{ fmtRate(monData.net_tx) }}</div>
                </div>
              </div>
              <!-- 流量迷你曲线图（SVG） -->
              <div class="spark-chart" v-if="netHist.length > 1">
                <svg :viewBox="`0 0 ${netHist.length} 30`" preserveAspectRatio="none">
                  <polyline :points="netRxPoints" fill="none" stroke="#00b42a" stroke-width="1.5" />
                  <polyline :points="netTxPoints" fill="none" stroke="#3491fa" stroke-width="1.5" />
                </svg>
                <div class="spark-legend">
                  <span><i class="dot rx"></i>下载</span>
                  <span><i class="dot tx"></i>上传</span>
                  <span class="total">累计 ↓{{ fmtKB(monData.net_rx_total_kb || 0) }} ↑{{ fmtKB(monData.net_tx_total_kb || 0) }}</span>
                </div>
              </div>
              <div class="mon-kv"><span>TCP 连接</span><b>{{ monData.net_conns ?? '-' }}</b></div>
            </div>

            <!-- 磁盘：占用 + 读写速率 -->
            <div class="dash-section">
              <div class="dash-title">磁盘 <span class="dash-val">{{ pct(monData.disk_percent) }}%</span></div>
              <div class="bar-wrap">
                <div class="bar-fill" :style="{ width: Math.min(100, monData.disk_percent || 0) + '%', background: barColor(monData.disk_percent || 0) }"></div>
              </div>
              <div class="mon-sub">{{ fmtKB(monData.disk_used_kb) }} / {{ fmtKB(monData.disk_total_kb) }}</div>
              <div class="disk-io" v-if="monData.disk_read_rate != null || monData.disk_write_rate != null">
                <span>读 {{ fmtRate(monData.disk_read_rate) }}</span>
                <span>写 {{ fmtRate(monData.disk_write_rate) }}</span>
              </div>
            </div>

            <!-- 进程 -->
            <div class="dash-section">
              <div class="dash-title">进程</div>
              <div class="proc-grid">
                <div class="proc-item">
                  <div class="proc-val">{{ monData.procs ?? '-' }}</div>
                  <div class="proc-label">总进程</div>
                </div>
                <div class="proc-item">
                  <div class="proc-val">{{ monData.running_procs ?? '-' }}</div>
                  <div class="proc-label">运行中</div>
                </div>
                <div class="proc-item">
                  <div class="proc-val">{{ monData.blocked_procs ?? '-' }}</div>
                  <div class="proc-label">阻塞</div>
                </div>
              </div>
            </div>

            <!-- 运行状态 -->
            <div class="dash-section">
              <div class="mon-kv"><span>运行时长</span><b>{{ uptime(monData.uptime_sec) }}</b></div>
              <div class="mon-kv"><span>延迟</span><b>{{ monData.latency_ms != null ? monData.latency_ms.toFixed(0) + 'ms' : '-' }}</b></div>
            </div>
          </template>
          <t-empty v-else description="暂无数据" />
        </t-loading>
      </div>
    </div>
    </div>

    

    <!-- 命令历史面板（单窗口模式显示在右侧） -->
    <div v-if="layout === '1' && showHistory" class="cmd-history">
      <div class="cmd-history-head">
        <span>命令历史（当前会话）</span>
        <t-icon name="close" @click="showHistory = false" />
      </div>
      <div class="cmd-history-list">
        <div
          v-for="(cmd, i) in cmdHistory"
          :key="i"
          class="cmd-history-item"
          @click="insertCmd(cmd)"
          :title="cmd"
        >{{ cmd }}</div>
        <div v-if="cmdHistory.length === 0" class="cmd-empty">暂无命令记录</div>
      </div>
      <div class="cmd-history-foot">
        <t-button size="small" theme="default" @click="loadCmdHistory" :loading="loadingHistory">
          读取 .bash_history
        </t-button>
      </div>
    </div>

    <!-- 挂起会话列表 -->
    <t-dialog v-model:visible="showSuspended" header="挂起的会话" width="720px" :footer="false">
      <t-alert theme="info" style="margin-bottom: 12px">
        挂起的会话在服务端保持运行，重新进入可恢复输出与进程状态。
      </t-alert>
      <t-table :data="suspendedSessions" :columns="suspColumns" row-key="id" hover size="small">
        <template #agent="{ row }">{{ row.agent_id }}</template>
        <template #created="{ row }">{{ formatMs(row.created_at) }}</template>
        <template #status="{ row }">
          <t-tag v-if="row.attached" theme="success" variant="light" size="small">已附着</t-tag>
          <t-tag v-else theme="warning" variant="light" size="small">挂起中</t-tag>
        </template>
        <template #op="{ row }">
          <t-space break-line>
            <t-link theme="primary" hover="color" @click="resumeSession(row)">恢复</t-link>
            <t-link theme="danger" hover="color" @click="killSession(row)">关闭</t-link>
          </t-space>
        </template>
      </t-table>
    </t-dialog>

    <!-- 关闭标签确认 -->
    <t-dialog v-model:visible="showCloseConfirm" header="关闭标签" @confirm="doCloseTab" @cancel="() => { showCloseConfirm = false; pendingCloseTabId = ''; }">
      <p>确定关闭标签 <b>{{ closingTabTitle }}</b> 吗？</p>
      <p class="warn-text" v-if="closingTabPanes > 0">
        该标签内有 {{ closingTabPanes }} 个活跃终端会话，关闭后其中的进程将被终止。
      </p>
      <t-checkbox v-model="closeKeepRunning">挂起会话（进程继续运行，稍后可在"挂起会话"中恢复）</t-checkbox>
    </t-dialog>
  </div>
</template>

<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, reactive, ref } from 'vue';
// 直接静态 import @codemirror/*：确保全应用唯一实例（不经中间层，杜绝多 chunk 多实例）
import { EditorView, keymap, lineNumbers, highlightActiveLineGutter } from '@codemirror/view';
import { EditorState } from '@codemirror/state';
import { oneDark } from '@codemirror/theme-one-dark';
import { markdown } from '@codemirror/lang-markdown';
import { defaultKeymap, history, historyKeymap } from '@codemirror/commands';
import { syntaxHighlighting, defaultHighlightStyle, bracketMatching } from '@codemirror/language';
import { useRoute } from 'vue-router';
import { MessagePlugin } from 'tdesign-vue-next';
import dayjs from 'dayjs';

import { listAgents } from '@/api/agent';

interface Pane {
  id: string;
  agentId: string;
  state: 'closed' | 'connecting' | 'open';
  ws: WebSocket | null;
  term: any;
  fit: any;
  el?: HTMLElement;
  suspend: boolean;
  sessionId: string;
  cwd: string;
  displayName?: string; // 缓存的主机名(ip)，连接后固定显示，避免反查失败回退 SHA
  remark?: string; // 用户自定义备注名，优先显示
}
interface Tab { id: string; title: string; panes: string[]; customTitle?: string }
interface SuspendedItem {
  id: string; agent_id: string; created_at: number; updated_at: number;
  suspended: boolean; attached: boolean; buffered: number;
  remark?: string; // 用户备注名
}

const route = useRoute();
const agents = ref<{ label: string; value: string }[]>([]);
const loadingAgents = ref(false);
const layout = ref<'1' | '2v' | '2h' | '4'>('1'); // 默认单屏

const tabs = ref<Tab[]>([]);
const activeTabId = ref('');
const panes = ref<Pane[]>([]);
const gridEl = ref<HTMLElement>();

const isFullscreen = ref(false);
const showHistory = ref(false);
const cmdHistory = ref<string[]>([]);
const loadingHistory = ref(false);
const showSuspended = ref(false);
/* 资源监控侧栏 */
const showMonitor = ref(false);
const monData = ref<any>(null);

/** 仪表盘：CPU/内存/磁盘 三个核心指标 */
const gauges = computed(() => {
  if (!monData.value) return [];
  return [
    { label: 'CPU', value: Number(monData.value.cpu_percent) || 0 },
    { label: '内存', value: Number(monData.value.mem_percent) || 0 },
    { label: '磁盘', value: Number(monData.value.disk_percent) || 0 },
  ];
});

/* 内存百分比计算 */
const memUsedPct = computed(() => {
  const m = monData.value;
  if (!m?.mem_total_kb) return 0;
  return Math.min(100, ((m.mem_used_kb || 0) / m.mem_total_kb) * 100);
});
const memCachedPct = computed(() => {
  const m = monData.value;
  if (!m?.mem_total_kb || !m?.mem_cached_kb) return 0;
  return Math.min(100 - memUsedPct.value, (m.mem_cached_kb / m.mem_total_kb) * 100);
});

/* 网络历史采样（用于迷你曲线图） */
const NET_HIST_MAX = 60;
const netHist = ref<Array<{ rx: number; tx: number }>>([]);
let netHistTimer: number | undefined;

function pushNetHist() {
  const m = monData.value;
  if (!m) return;
  netHist.value.push({ rx: m.net_rx || 0, tx: m.net_tx || 0 });
  if (netHist.value.length > NET_HIST_MAX) netHist.value.shift();
}

/* 曲线图 SVG polyline points */
const netRxPoints = computed(() => {
  if (!netHist.value.length) return '';
  const max = Math.max(...netHist.value.map((h) => h.rx), 1);
  return netHist.value.map((h, i) => `${i},${30 - (h.rx / max) * 28 - 1}`).join(' ');
});
const netTxPoints = computed(() => {
  if (!netHist.value.length) return '';
  const max = Math.max(...netHist.value.map((h) => h.tx), 1);
  return netHist.value.map((h, i) => `${i},${30 - (h.tx / max) * 28 - 1}`).join(' ');
});
const monLoading = ref(false);
let monTimer: number | undefined;
/* 文件传输：在线编辑 */
const tShowEditor = ref(false);
const tSearchQuery = ref('');
const tEditLoading = ref(false);
const tEditSaving = ref(false);
const tEditPath = ref('');
const tEditContent = ref('');
const tEditBinary = ref(false);
const tEditTruncated = ref(false);
const tEditMarkdown = ref(false);
const tEditMode = ref<'view' | 'edit'>('view');
const tOriginalContent = ref('');
const tShowDiff = ref(false);
const tEditorEl = ref<HTMLElement>();
let tCmView: any = null;

/* 文件传输右键菜单 */
const tfCtxMenu = reactive({ show: false, x: 0, y: 0, file: null as any });
/** 菜单挂载目标：全屏时用 .term-page（全屏元素创建新层叠上下文，body 下菜单不可见） */
const fsTick = ref(0);
const ctxMenuTarget = computed(() => {
  void fsTick.value; // fullscreenchange 时触发重算
  return document.fullscreenElement ? '.term-page' : 'body';
});
onMounted(() => document.addEventListener('fullscreenchange', () => fsTick.value++));
onUnmounted(() => document.removeEventListener('fullscreenchange', () => fsTick.value++));
function onTfCtxMenu(e: MouseEvent, f: any) {
  tfCtxMenu.show = true;
  tfCtxMenu.x = e.clientX;
  tfCtxMenu.y = e.clientY;
  tfCtxMenu.file = f;
}
function hideTfCtxMenu() { tfCtxMenu.show = false; tfCtxMenu.file = null; }

/* 文件传输侧栏 */
const showTransfer = ref(false);
const showShortcuts = ref(false);
const terminalCwd = ref('/');
/* 终端风格配置（本地记忆） */
const termStyle = ref(localStorage.getItem('ctc_term_style') || 'default');
const focusedPaneId = ref('');

/* 终端风格预设 */
const TERM_STYLES: Record<string, any> = {
  default: {
    name: '默认',
    fontFamily: 'Menlo, Monaco, "Courier New", monospace',
    fontSize: 13,
    cursorBlink: true,
    theme: { background: '#1e1e1e', foreground: '#d4d4d4', cursor: '#00ff00' },
  },
  retro: {
    name: '复古',
    fontFamily: '"Courier New", Courier, monospace',
    fontSize: 14,
    cursorBlink: true,
    theme: { background: '#000300', foreground: '#00ff41', cursor: '#00ff41' },
  },
  light: {
    name: '明亮',
    fontFamily: '"Source Code Pro", Menlo, monospace',
    fontSize: 13,
    cursorBlink: true,
    theme: { background: '#ffffff', foreground: '#333333', cursor: '#000000' },
  },
  ocean: {
    name: '海洋',
    fontFamily: '"Fira Code", "DejaVu Sans Mono", monospace',
    fontSize: 13,
    cursorBlink: false,
    theme: { background: '#0b2239', foreground: '#d9e9f4', cursor: '#4fc3f7' },
  },
};

/** 切换终端风格并应用到所有 pane */
function setTermStyle(key: string) {
  termStyle.value = key;
  localStorage.setItem('ctc_term_style', key);
  const cfg = TERM_STYLES[key] || TERM_STYLES.default;
  panes.value.forEach((pane) => {
    if (pane.term) {
      pane.term.options.fontFamily = cfg.fontFamily;
      pane.term.options.fontSize = cfg.fontSize;
      pane.term.options.theme = cfg.theme;
      pane.term.options.cursorBlink = cfg.cursorBlink;
    }
  });
  MessagePlugin.success(`已切换为「${cfg.name}」风格`);
}
const transferPath = ref('/');
const transferFiles = ref<any[]>([]);
const transferView = ref<'list' | 'icon'>(localStorage.getItem('ctc_tf_view') === 'icon' ? 'icon' : 'list');
const selectedTfPath = ref('');
const transferLoading = ref(false);
const transferUploading = ref(false);
const uploadProgress = ref(0);
const suspendedMap = ref<Map<string, SuspendedItem>>(new Map());
const suspendedSessions = ref<SuspendedItem[]>([]);
const showCloseConfirm = ref(false);
const pendingCloseTabId = ref('');
const closeKeepRunning = ref(false);

const agentOptions = computed(() => agents.value);
const activeTab = computed(() => tabs.value.find((t) => t.id === activeTabId.value));
const suspendedCount = computed(() => suspendedSessions.value.length);
const closingTabTitle = computed(() => tabs.value.find((t) => t.id === pendingCloseTabId.value)?.title || '');
const closingTabPanes = computed(() => {
  const tab = tabs.value.find((t) => t.id === pendingCloseTabId.value);
  if (!tab) return 0;
  return tab.panes.filter((pid) => {
    const p = panes.value.find((x) => x.id === pid);
    return p && p.state === 'open';
  }).length;
});

const paneCount = computed(() => (layout.value === '1' ? 1 : layout.value === '4' ? 4 : 2));
/** 是否单窗格（决定面板内嵌 or 浮层） */
const isSinglePane = computed(() => layout.value === '1');
const panesForRender = computed(() => {
  const tab = activeTab.value;
  if (!tab) return [];
  return tab.panes
    .map((id) => panes.value.find((p) => p.id === id))
    .filter(Boolean)
    .slice(0, paneCount.value) as Pane[];
});

/** 所有 tab 的所有 pane 常驻（v-show 控制显示，切 tab 不销毁 DOM，xterm 文本保留） */
const allPanes = computed(() => {
  const seen = new Set<string>();
  const out: Pane[] = [];
  for (const tab of tabs.value) {
    for (const pid of tab.panes) {
      if (!seen.has(pid)) {
        seen.add(pid);
        const p = panes.value.find((x) => x.id === pid);
        if (p) out.push(p);
      }
    }
  }
  return out;
});

function readTk(): string {
  try {
    return window.localStorage.getItem('ctc_' + 'tok' + 'en') || '';
  } catch {
    return '';
  }
}

let uid = 0;
const nextId = (p: string) => `${p}-${++uid}`;

function terminalUrl(agentId: string, sessionId?: string, suspend?: boolean) {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  const tk = readTk();
  let url = `${proto}://${location.host}/ws/terminal?agent_id=${encodeURIComponent(agentId)}&token=${tk}`;
  if (sessionId) url += `&session_id=${encodeURIComponent(sessionId)}`;
  if (suspend) url += `&suspend=1`;
  return url;
}

async function loadAgents() {
  loadingAgents.value = true;
  try {
    const list = await listAgents();
    agents.value = list
      .filter((a) => a.online && a.approval === 'approved')
      .map((a) => ({ label: `${a.hostname} (${a.ip})`, value: a.agent_id }));
  } finally {
    loadingAgents.value = false;
  }
}

async function fetchSuspended() {
  try {
  const tk = readTk();
    const r = await fetch('/api/terminals', { headers: { Authorization: 'Bearer ' + tk } });
    const d = await r.json();
    suspendedSessions.value = d.data || [];
    const m = new Map<string, SuspendedItem>();
    for (const item of suspendedSessions.value) m.set(item.id, item);
    suspendedMap.value = m;
  } catch { /* ignore */ }
}

function paneStyle(idx: number) {
  const n = paneCount.value;
  if (n === 1) return { gridColumn: '1 / -1', gridRow: '1 / -1' };
  if (layout.value === '2v') return { gridColumn: `${idx + 1}`, gridRow: '1' };
  if (layout.value === '2h') return { gridColumn: '1', gridRow: `${idx + 1}` };
  const col = (idx % 2) + 1;
  const row = Math.floor(idx / 2) + 1;
  return { gridColumn: `${col}`, gridRow: `${row}` };
}

/** 拖拽调整分屏比例（在两条轴线上做） */
function makeResizable() {
  const grid = gridEl.value;
  if (!grid) return;
  // 移除旧分隔条
  grid.querySelectorAll('.grid-divider').forEach((d) => d.remove());

  const cols = layout.value === '2v' ? 2 : layout.value === '4' ? 2 : 1;
  const rows = layout.value === '2h' ? 2 : layout.value === '4' ? 2 : 1;
  if (cols <= 1 && rows <= 1) return;

  // 竖直分隔线（左右分屏 / 四分屏）
  if (cols === 2) {
    addDivider(grid, 'vertical', (deltaPx) => {
      const total = grid.clientWidth;
      const pct = 50 + (deltaPx / total) * 50;
      grid.style.gridTemplateColumns = `${pct}% 4px 1fr`;
    });
  }
  // 水平分隔线（上下分屏 / 四分屏）
  if (rows === 2) {
    addDivider(grid, 'horizontal', (deltaPx) => {
      const total = grid.clientHeight;
      const pct = 50 + (deltaPx / total) * 50;
      grid.style.gridTemplateRows = `${pct}% 4px 1fr`;
    });
  }
}

function addDivider(grid: HTMLElement, dir: 'vertical' | 'horizontal', onDrag: (delta: number) => void) {
  const d = document.createElement('div');
  d.className = `grid-divider grid-divider-${dir}`;
  d.style.cssText = dir === 'vertical'
    ? 'position:absolute;left:50%;top:0;bottom:0;width:2px;cursor:col-resize;background:rgba(255,255,255,0.06);z-index:10;opacity:0.5;'
    : 'position:absolute;left:0;right:0;top:50%;height:2px;cursor:row-resize;background:rgba(255,255,255,0.06);z-index:10;opacity:0.5;';
  d.addEventListener('mousedown', (e) => {
    e.preventDefault();
    const start = dir === 'vertical' ? e.clientX : e.clientY;
    const onMove = (ev: MouseEvent) => {
      const cur = dir === 'vertical' ? ev.clientX : ev.clientY;
      onDrag(cur - start);
    };
    const onUp = () => {
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
      // 拖拽结束触发 fit
      panesForRender.value.forEach((p) => p.fit?.fit());
    };
    document.addEventListener('mousemove', onMove);
    document.addEventListener('mouseup', onUp);
  });
  grid.appendChild(d);
}

const suspColumns = [
  { colKey: 'agent', title: '主机', width: 200, ellipsis: true },
  { colKey: 'created', title: '创建时间', width: 160 },
  { colKey: 'status', title: '状态', width: 90 },
  { colKey: 'buffered', title: '缓冲', width: 90 },
  { colKey: 'op', title: '操作', width: 130, fixed: 'right' as const },
];

function formatMs(ms?: number) {
  return ms ? dayjs(ms).format('MM-DD HH:mm:ss') : '-';
}

function setTermEl(id: string, el: HTMLElement) {
  const p = panes.value.find((x) => x.id === id);
  if (p && el) {
    p.el = el;
    // 拖拽上传：支持把文件从桌面拖进终端窗口
    if (!(el as any).__dropBound) {
      (el as any).__dropBound = true;
      el.addEventListener('dragover', (e: DragEvent) => {
        e.preventDefault();
        el.style.outline = '2px dashed var(--td-brand-color)';
      });
      el.addEventListener('dragleave', () => {
        el.style.outline = '';
      });
      el.addEventListener('drop', async (e: DragEvent) => {
        e.preventDefault();
        el.style.outline = '';
        const files = Array.from(e.dataTransfer?.files || []);
        if (!files.length) return;
        const agent = currentPaneAgent.value;
        if (!agent) {
          MessagePlugin.warning('请先选择主机');
          return;
        }
        MessagePlugin.info(`开始上传 ${files.length} 个文件到 ${transferPath.value}...`);
        try {
          const { writeFile } = await import('@/api/agent');
          for (const f of files) {
            const buf = await f.arrayBuffer();
            const bytes = new Uint8Array(buf);
            let bin = '';
            const STEP = 8192;
            for (let i = 0; i < bytes.length; i += STEP) {
              bin += String.fromCharCode.apply(null, Array.from(bytes.subarray(i, i + STEP)) as any);
            }
            const target = tJoin(transferPath.value, f.name);
            await writeFile(agent, target, btoa(bin));
          }
          MessagePlugin.success(`已上传 ${files.length} 个文件`);
          if (showTransfer.value) tLoad();
        } catch (err: any) {
          MessagePlugin.error(err.message || '上传失败');
        }
      });
    }
  }
}

async function ensureXterm(pane: Pane) {
  // DOM 常驻不销毁，term 存在即返回
  if (pane.term) return;
  const [{ Terminal }, { FitAddon }] = await Promise.all([
    import('@xterm/xterm'),
    import('@xterm/addon-fit'),
  ]);
  await import('@xterm/xterm/css/xterm.css');
  const styleCfg = TERM_STYLES[termStyle.value] || TERM_STYLES.default;
  const t = new Terminal({
    cursorBlink: styleCfg.cursorBlink,
    fontSize: styleCfg.fontSize,
    fontFamily: styleCfg.fontFamily,
    theme: styleCfg.theme,
    scrollback: 5000,
  });
  const fit = new FitAddon();
  t.loadAddon(fit);
  if (pane.el) {
    t.open(pane.el);
    (pane as any).__termHost = pane.el;
    fit.fit();
    // 右键复制/粘贴挂载到 pane 容器（确保事件绑定到正确的 DOM）
    attachCopyMenu(pane);
    // 把 doPaste 挂到 pane 上，供 attachCustomKeyEventHandler 拦截 Ctrl+V 时调用
    (pane as any).__doPaste = () => {
      const sel = t.getSelection?.() || '';
      if (sel) {
        // 有选中时先复制
        if (navigator.clipboard && window.isSecureContext) {
          navigator.clipboard.writeText(sel).then(() => {
            MessagePlugin.success('已复制 ' + sel.length + ' 字符');
          }).catch(() => {});
        }
        return;
      }
      // 无选中时粘贴
      doPasteFromClipboard(pane);
    };
  }
  /* 命令记录：按回车切分，去掉控制字符 */
  let inputBuf = '';
  t.onData((d: string) => {
    // Tab 键：必须作为二进制帧发送，避免服务端误判为 JSON 控制消息
    if (d === '\t') {
      if (pane.ws && pane.ws.readyState === WebSocket.OPEN) {
        pane.ws.send(new Uint8Array([0x09])); // Tab = 0x09
      }
      return;
    }
    if (pane.ws && pane.ws.readyState === WebSocket.OPEN) pane.ws.send(d);
    for (const ch of d) {
      if (ch === '\r' || ch === '\n') {
        recordCmd(inputBuf);
        inputBuf = '';
      } else if (ch === '\x7f' || ch === '\b') {
        inputBuf = inputBuf.slice(0, -1);
      } else if (ch >= ' ') {
        inputBuf += ch;
      }
    }
  });
  /* 右键：选中文本→复制；无选中→粘贴 */
  t.element?.addEventListener('contextmenu', (e: MouseEvent) => {
    e.preventDefault();
    e.stopPropagation();
    const sel = t.getSelection?.() || '';
    if (sel) {
      navigator.clipboard.writeText(sel).then(() => {
        MessagePlugin.success('已复制 ' + sel.length + ' 字符');
      }).catch(() => {});
    } else {
      doPasteFromClipboard(pane);
    }
  });
  /* Ctrl+V / Ctrl+Shift+V 拦截：强制走我们的粘贴逻辑 */
  t.attachCustomKeyEventHandler?.((event: KeyboardEvent) => {
    // Ctrl+V：阻止 xterm 默认粘贴，走 doPaste
    if (event.type === 'keydown' && (event.ctrlKey || event.metaKey) && event.key === 'v') {
      event.preventDefault();
      event.stopPropagation();
      (pane as any).__doPaste?.();
      return false;
    }
    // Ctrl+数字：在 xterm 层直接切换窗格并阻止浏览器默认（Chrome 会用 Ctrl+1-9 切标签页，
    // 必须 preventDefault + return false 才能到达切换逻辑，解决 Ctrl+3/4 切不到下方窗格）
    if (event.type === 'keydown' && event.ctrlKey && !event.shiftKey && !event.altKey
        && event.key >= '1' && event.key <= '9') {
      event.preventDefault();
      event.stopPropagation();
      const panes = panesForRender.value;
      const idx = parseInt(event.key, 10) - 1;
      if (idx < panes.length) focusPane(panes[idx]);
      return false;
    }
    // Ctrl+`：xterm 层循环切换（更可靠，不被浏览器拦截）
    if (event.type === 'keydown' && event.ctrlKey && !event.altKey
        && (event.key === '`' || event.code === 'Backquote')) {
      event.preventDefault();
      event.stopPropagation();
      const panes = panesForRender.value;
      const curIdx = panes.findIndex((p) => p.id === focusedPaneId.value);
      const step = event.shiftKey ? -1 : 1;
      focusPane(panes[((curIdx < 0 ? 0 : curIdx) + step + panes.length) % panes.length]);
      return false;
    }
    return true;
  });

  t.onBinary((d: string) => {
    if (pane.ws && pane.ws.readyState === WebSocket.OPEN) {
      const buf = new Uint8Array(d.length);
      for (let i = 0; i < d.length; i++) buf[i] = d.charCodeAt(i) & 255;
      pane.ws.send(buf);
    }
  });
  pane.term = t;
  pane.fit = fit;
}

function makePane(agentId: string): Pane {
  const id = nextId('pane');
  const p: Pane = { id, agentId, state: 'closed', ws: null, term: null, fit: null, suspend: false, sessionId: '', cwd: '/' };
  panes.value.push(p);
  return p;
}

async function connectPane(pane: Pane) {
  if (!pane.agentId) return;
  if (pane.ws && pane.ws.readyState === WebSocket.OPEN) return;
  await ensureXterm(pane);
  pane.state = 'connecting';
  const ws = new WebSocket(terminalUrl(pane.agentId, pane.sessionId || undefined, pane.suspend));
  ws.binaryType = 'arraybuffer';
  pane.ws = ws;
  ws.onopen = () => {
    pane.state = 'open';
    pane.term?.focus();
    pane.fit?.fit();
  };
  ws.onmessage = (ev) => {
    if (typeof ev.data === 'string') {
      // 首帧是 session 元信息
      try {
        const m = JSON.parse(ev.data);
        if (m.type === 'session' && m.session_id) {
          pane.sessionId = m.session_id;
          return;
        }
      } catch { /* 非 JSON，作为终端输出 */ }
      pane.term?.write(ev.data);
      trackCwd(pane, ev.data);
    } else {
      const bytes = new Uint8Array(ev.data as ArrayBuffer);
      pane.term?.write(bytes);
      // 解析提示符，同步工作目录到文件传输面板
      try { trackCwd(pane, new TextDecoder().decode(bytes)); } catch { /* ignore */ }
    }
  };
  ws.onclose = () => {
    pane.state = 'closed';
    if (!pane.suspend) {
      pane.term?.writeln('\r\n\x1b[33m[连接已关闭]\x1b[0m');
    }
  };
  ws.onerror = () => { pane.state = 'closed'; };
}

async function disconnectPane(pane: Pane) {
  if (pane.ws) {
    // 通知服务端是挂起还是关闭
    try {
      if (pane.suspend && pane.ws.readyState === WebSocket.OPEN) {
        pane.ws.send(JSON.stringify({ type: 'suspend', cols: 1, rows: 1, remark: pane.remark || '' }));
      }
      pane.ws.close();
    } catch { /* ignore */ }
    pane.ws = null;
  }
  pane.state = 'closed';
}

function reconnectPane(pane: Pane) {
  pane.term?.reset();
  disconnectPane(pane).then(() => connectPane(pane));
}

/** 主机下拉 + 挂起会话下拉统一选项 */
const paneSelectOptions = computed(() => {
  const opts: { label: string; value: string; group?: string }[] = [];
  // 在线主机
  agents.value.forEach((a) => opts.push({ label: `🖥 ${a.label}`, value: `agent:${a.value}`, group: 'online' }));
  // 挂起会话
  suspendedSessions.value.forEach((s) => {
    opts.push({
      label: `⏸ ${s.remark || s.agent_id} · ${dayjs(s.created_at).format('MM-DD HH:mm')}`,
      value: `sess:${s.id}`,
      group: 'suspended',
    });
  });
  return opts;
});

function shortSid(id: string) { return id.slice(0, 8); }

/** pane 标题显示：agentId 反查主机名(ip)，而非 SHA */
/** 下拉框选中值：与 options 的 agent:/sess: 前缀格式对齐（否则匹配不到显示原始 SHA） */
function paneSelectValue(pane: Pane): string {
  if (pane.sessionId) return `sess:${pane.sessionId}`;
  if (pane.agentId) return `agent:${pane.agentId}`;
  return '';
}

const paneTitle = (pane: Pane): string => {
  // 备注名优先（用户自定义，区分主机）
  if (pane.remark) return pane.remark;
  // displayName：选主机时已缓存 hostname(ip)，连接后 sessionId 非空也不变 SHA
  if (pane.displayName) return pane.displayName;
  if (pane.sessionId) return `⏸ ${shortSid(pane.sessionId)}`;
  if (!pane.agentId) return '选择目标';
  const aid = pane.agentId.startsWith('agent:') ? pane.agentId.slice(6) : pane.agentId;
  const opt = paneSelectOptions.value.find((o: any) => o.value === `agent:${aid}`);
  if (opt) return String(opt.label).replace(/^\S+\s/, '');
  const a = agents.value.find((x: any) => x.value === aid);
  return a ? a.label : aid;
};

/** pane 切换目标：在线主机 或 挂起会话 */
function onPaneTargetChange(pane: Pane, v: string) {
  const [kind, id] = v.split(':');
  if (kind === 'agent') {
    pane.agentId = id;
    pane.sessionId = '';
    pane.suspend = false;
    const opt = paneSelectOptions.value.find((o: any) => o.value === `agent:${id}`);
    pane.displayName = opt ? String(opt.label).replace(/^\S+\s/, '') : id;
    onPaneAgentChange(pane);
    nextTick(refreshSidePanels);
  } else if (kind === 'sess') {
    // 切到挂起会话：恢复会话
    const item = suspendedMap.value.get(id);
    if (!item) {
      MessagePlugin.warning('会话不存在或已关闭');
      return;
    }
    pane.agentId = item.agent_id;
    // 恢复时优先用挂起时保存的备注（服务端返回），其次原窗口备注
    if (item.remark) pane.remark = item.remark;
    pane.displayName = pane.remark || `⏸ ${shortSid(id)}`;
    pane.sessionId = item.id;
    pane.suspend = true;
    pane.state = 'closed';
    pane.term?.reset();
    disconnectPane(pane).then(() => connectPane(pane));
    MessagePlugin.success('正在恢复挂起会话...');
    fetchSuspended();
  }
}

function onPaneAgentChange(pane: Pane) {
  pane.sessionId = '';
  pane.term?.reset();
  disconnectPane(pane).then(() => connectPane(pane));
}

async function toggleSuspend(pane: Pane, on: boolean) {
  pane.suspend = on;
  if (pane.ws && pane.ws.readyState === WebSocket.OPEN) {
    try {
      pane.ws.send(JSON.stringify({ type: 'suspend', cols: on ? 1 : 0, rows: 0, remark: pane.remark || '' }));
    } catch { /* ignore */ }
  }
  MessagePlugin.info(on ? '已挂起（断开后进程继续运行）' : '已取消挂起');
}

function addTab() {
  const id = nextId('tab');
  const def = agents.value.find((a: any) => a.online) || agents.value[0];
  const defaultAgent = def?.value || '';
  const ids: string[] = [];
  for (let i = 0; i < Math.max(paneCount.value, 1); i++) {
    const pane = makePane(defaultAgent);
    // 默认创建时即缓存 displayName（hostname(ip)），连接后稳定显示
    if (def) pane.displayName = def.label;
    ids.push(pane.id);
  }
  tabs.value.push({ id, title: `标签 ${tabs.value.length + 1}`, panes: ids });
  activeTabId.value = id;
  nextTick(() => {
    ids.slice(0, paneCount.value).forEach((pid) => {
      const p = panes.value.find((x) => x.id === pid);
      if (p) connectPane(p);
    });
    fitAll();
    nextTick(() => makeResizable());
  });
}

/** 双击标签改名 */
/** 终端窗口备注名：双击标题弹窗修改 */
function renamePane(pane: Pane) {
  const name = window.prompt('窗口备注名（留空清除备注，显示主机名）：', pane.remark || '');
  if (name === null) return;
  pane.remark = name.trim() || undefined;
}

function renameTab(tab: Tab) {
  const name = window.prompt('自定义标签名（留空恢复默认）：', tab.customTitle || tab.title);
  if (name === null) return;
  tab.customTitle = name.trim() || undefined;
}

/** 挂起标签内全部会话 */
async function suspendTab(tab: Tab) {
  for (const pid of tab.panes) {
    const p = panes.value.find((x) => x.id === pid);
    if (p) {
      p.suspend = true;
      await toggleSuspend(p, true);
    }
  }
  MessagePlugin.success(`标签「${tab.customTitle || tab.title}」内 ${tab.panes.length} 个会话已挂起`);
}

/** 按原配置恢复挂起的标签（重新连接所有 pane） */
async function resumeTab(tab: Tab) {
  for (const pid of tab.panes) {
    const p = panes.value.find((x) => x.id === pid);
    if (p) {
      p.suspend = false;
      await disconnectPane(p);
      await connectPane(p);
    }
  }
  MessagePlugin.success(`已按原配置恢复标签「${tab.customTitle || tab.title}」`);
}

function confirmCloseTab(id: string) {
  pendingCloseTabId.value = id;
  closeKeepRunning.value = false;
  showCloseConfirm.value = true;
}

function doCloseTab() {
  const id = pendingCloseTabId.value;
  showCloseConfirm.value = false;
  pendingCloseTabId.value = '';
  const idx = tabs.value.findIndex((t) => t.id === id);
  if (idx < 0) return;
  const tab = tabs.value[idx];
  tab.panes.forEach((pid) => {
    const p = panes.value.find((x) => x.id === pid);
    if (p) {
      if (closeKeepRunning.value) {
        p.suspend = true;
      }
      try { disconnectPane(p); } catch (e) { /* ignore */ }
      try { p.term?.dispose(); } catch (e) { /* ignore */ }
    }
  });
  panes.value = panes.value.filter((p) => !tab.panes.includes(p.id));
  tabs.value.splice(idx, 1);
  if (activeTabId.value === id) {
    activeTabId.value = tabs.value[Math.max(0, idx - 1)]?.id || '';
  }
}

function setLayout(l: '1' | '2v' | '2h' | '4') {
  layout.value = l;
  const grid = gridEl.value;
  if (grid) {
    grid.style.gridTemplateColumns = '';
    grid.style.gridTemplateRows = '';
  }
  const tab = activeTab.value;
  if (!tab) return;
  while (tab.panes.length < paneCount.value) {
    tab.panes.push(makePane(agents.value[0]?.value || '').id);
  }
  nextTick(() => {
    tab.panes.slice(0, paneCount.value).forEach((pid) => {
      const p = panes.value.find((x) => x.id === pid);
      if (p && p.el && p.term) {
        p.term.open(p.el);
      }
      if (p && p.state === 'closed' && p.agentId) connectPane(p);
    });
    fitAll();
    nextTick(() => makeResizable());
  });
}

function fitAll() {
  panesForRender.value.forEach((p) => {
    try { p.fit?.fit(); } catch { /* ignore */ }
  });
}

function onResize() { fitAll(); }

async function resumeSession(item: SuspendedItem) {
  // 新开一个标签并附着到该会话
  const id = nextId('tab');
  const pane = makePane(item.agent_id);
  pane.sessionId = item.id;
  pane.suspend = true;
  tabs.value.push({ id, title: `恢复 ${item.agent_id}`, panes: [pane.id] });
  activeTabId.value = id;
  showSuspended.value = false;
  await nextTick();
  connectPane(pane);
}

async function killSession(item: SuspendedItem) {
  try {
    const r = await fetch(`/api/terminals/${item.id}/close`, {
      method: 'POST',
      headers: { Authorization: 'Bearer ' + readTk() },
    });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    MessagePlugin.success('会话已关闭');
    fetchSuspended();
  } catch (e: any) {
    MessagePlugin.error(e.message || '关闭失败');
  }
}

/** 全屏整个终端页 */
function toggleFullscreen() {
  const el = document.querySelector('.term-page') as HTMLElement | null;
  if (!el) return;
  if (!document.fullscreenElement) {
    el.requestFullscreen?.().then(() => {
      isFullscreen.value = true;
      setTimeout(fitAll, 150);
    }).catch(() => {});
  } else {
    document.exitFullscreen?.().then(() => {
      isFullscreen.value = false;
      setTimeout(fitAll, 150);
    }).catch(() => {});
  }
}

/** 单个窗格全屏（四分屏/双分屏下也可用） */
function fullscreenPane(pane: Pane) {
  const el = pane.el?.closest('.pane') as HTMLElement | null;
  if (!el) return;
  if (!document.fullscreenElement) {
    el.requestFullscreen?.().then(() => {
      setTimeout(() => { pane.fit?.fit(); pane.term?.focus(); }, 150);
    }).catch(() => {});
  } else {
    document.exitFullscreen?.().then(() => setTimeout(fitAll, 150)).catch(() => {});
  }
}

/** 把历史命令插入终端（不自动回车，便于编辑） */
function insertCmd(cmd: string) {
  const pane = panesForRender.value[0];
  if (!pane || !pane.ws || pane.ws.readyState !== WebSocket.OPEN) {
    MessagePlugin.warning('终端未连接');
    return;
  }
  pane.ws.send(cmd);
  pane.term?.focus();
}

/** 从终端输出中解析当前工作目录。
 * 提示符形态多样：
 *   [root@host /path]#     （带方括号）
 *   root@host:/path#       （不带）
 *   root@host:/path $ 
 *   root@host ~ #
 * 用 ANSI 转义剥离后按最后一个空格前的段取路径。
 */
const paneCwdBuf = new Map<string, string>();

function trackCwd(pane: Pane | null, chunk: string) {
  if (!pane) return;
  let buf = (paneCwdBuf.get(pane.id) || '') + chunk;
  if (buf.length > 8192) buf = buf.slice(-4096);

  // 剥离 ANSI 转义序列（终端颜色、光标控制）
  const clean = buf.replace(/\x1b\[[0-9;?]*[a-zA-Z]/g, '').replace(/\x1b\][^\x07]*\x07/g, '');

  // 匹配形如 [...]# 或 [...]$ 的提示符：提取最后一个 "xxx:/path" 或 "user@host:/path"
  const m = clean.match(/([\w.@-]+)?[:~]([^#\s$\n]{1,256})[#$][\s]*$/);
  if (m) {
    let p = (m[2] || '').trim();
    // 如果路径本身是 ~ 或 ~ 开头，展开为 /root
    if (p === '~' || p.startsWith('~/')) p = p.replace(/^~/, '/root');
    if (p === ' ' || p === '') p = '/';
    if (p.startsWith('/')) {
      terminalCwd.value = p;
      // 同步当前 pane 的 cwd（多窗格各自独立）
      pane.cwd = p;
      // 如果文件传输面板已打开且跟随当前 pane，自动刷新到新路径
      if (showTransfer.value && pane.id === focusedPaneId.value) {
        transferPath.value = p;
        tLoad();
      }
    }
    paneCwdBuf.set(pane.id, '');
    return;
  }
  paneCwdBuf.set(pane.id, buf);
}

/** 记录执行过的命令（前端采集，按回车切分） */
function recordCmd(buf: string) {
  const t = buf.trim();
  if (!t) return;
  if (cmdHistory.value[0] === t) return; // 去重连续重复
  cmdHistory.value.unshift(t);
  if (cmdHistory.value.length > 200) cmdHistory.value.pop();
}

/** 右键复制选中文本 */
/** 从剪贴板粘贴到终端（HTTPS 用 Clipboard API，HTTP 用 execCommand 降级） */
/** 粘贴发送：将 \n 转为 \r（vim 期望回车），去除尾随换行避免多行粘贴错乱 */
function sendPaste(pane: Pane, txt: string) {
  if (!pane.ws || pane.ws.readyState !== WebSocket.OPEN || !txt) return;
  // 去除 CRLF，统一转 \r；去除末尾换行避免末尾多一空行
  const normalized = txt.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
  const trimmed = normalized.replace(/\n+$/, '');
  // 发送时 \n → \r（终端换行协议）
  const payload = trimmed.replace(/\n/g, '\r');
  pane.ws.send(payload);
}

async function doPasteFromClipboard(pane: Pane) {
  if (!pane.ws || pane.ws.readyState !== WebSocket.OPEN) {
    MessagePlugin.warning('终端未连接');
    return;
  }
  // HTTPS / localhost：Clipboard API
  if (navigator.clipboard && window.isSecureContext) {
    try {
      const txt = await navigator.clipboard.readText();
      if (txt) {
        sendPaste(pane, txt);
        MessagePlugin.success('已粘贴 ' + txt.length + ' 字符');
      }
      return;
    } catch { /* 降级 */ }
  }
  // HTTP 环境：execCommand('paste') 降级
  try {
    const ta = document.createElement('textarea');
    ta.style.cssText = 'position:fixed;left:-9999px;top:0;opacity:0;';
    ta.setAttribute('readonly', '');
    document.body.appendChild(ta);
    ta.select();
    ta.setSelectionRange(0, 0);
    const ok = document.execCommand('paste');
    const txt = ta.value;
    document.body.removeChild(ta);
    if (ok && txt) {
      sendPaste(pane, txt);
      MessagePlugin.success('已粘贴 ' + txt.length + ' 字符');
      return;
    }
  } catch { /* ignore */ }
  // 最终提示
  MessagePlugin.warning('HTTP 环境浏览器限制自动粘贴，请用 Ctrl+V');
  pane.term?.focus();
}

function attachCopyMenu(pane: Pane) {
  const el = pane.el;
  if (!el) return;

  /** 安全粘贴：HTTPS 用 Clipboard API，HTTP 用 execCommand 降级 */
  async function doPaste() {
    if (!pane.ws || pane.ws.readyState !== WebSocket.OPEN) {
      MessagePlugin.warning('终端未连接');
      return;
    }
    // 优先 Clipboard API（HTTPS/localhost）
    if (navigator.clipboard && window.isSecureContext) {
      try {
        const txt = await navigator.clipboard.readText();
        if (txt) sendPaste(pane, txt);
        return;
      } catch { /* 降级 */ }
    }
    // HTTP 环境：execCommand('paste') 被禁用，改为提示用户用 Ctrl+V
    // 但先尝试从 window 读剪贴板（某些浏览器允许）
    try {
      const ta = document.createElement('textarea');
      ta.style.cssText = 'position:fixed;left:-9999px;top:0;opacity:0;';
      ta.setAttribute('readonly', '');
      document.body.appendChild(ta);
      ta.select();
      ta.setSelectionRange(0, 0);
      const ok = document.execCommand('paste');
      const txt = ta.value;
      document.body.removeChild(ta);
      if (ok && txt) {
        sendPaste(pane, txt);
        MessagePlugin.success('已粘贴 ' + txt.length + ' 字符');
        return;
      }
    } catch { /* ignore */ }

    // 最终降级：显示提示，引导用户用 Ctrl+V
    MessagePlugin.warning('HTTP 环境浏览器限制自动粘贴，请用 Ctrl+V 粘贴');
    // 同时把焦点还给终端，方便用户直接 Ctrl+V
    pane.term?.focus();
  }

  /** 安全复制 */
  async function doCopy() {
    const sel = pane.term?.getSelection?.() || '';
    if (!sel) return false;
    if (navigator.clipboard && window.isSecureContext) {
      try {
        await navigator.clipboard.writeText(sel);
        MessagePlugin.success('已复制 ' + sel.length + ' 字符');
        return true;
      } catch { /* 降级 */ }
    }
    // 降级：临时 textarea + execCommand('copy')
    const ta = document.createElement('textarea');
    ta.value = sel;
    ta.style.cssText = 'position:fixed;left:-9999px;top:0;opacity:0;';
    document.body.appendChild(ta);
    ta.select();
    const ok = document.execCommand('copy');
    document.body.removeChild(ta);
    if (ok) MessagePlugin.success('已复制 ' + sel.length + ' 字符');
    else MessagePlugin.error('复制失败，请手动 Ctrl+C');
    return ok;
  }

  // 右键菜单：绑定到 pane 容器（canvas 会阻止冒泡，需要 capture 阶段）
  el.addEventListener('contextmenu', (e: MouseEvent) => {
    e.preventDefault();
    e.stopPropagation();
    const sel = pane.term?.getSelection?.() || '';
    if (sel) doCopy();
    else doPaste();
  }, true); // capture 阶段，确保 xterm canvas 不拦截

  // 同时绑定到 xterm 的 textarea（输入区域右键）
  const textEl = el.querySelector('.xterm-helper-textarea') as HTMLElement | null;
  if (textEl) {
    textEl.addEventListener('contextmenu', (e: MouseEvent) => {
      e.preventDefault();
      e.stopPropagation();
      const sel = pane.term?.getSelection?.() || '';
      if (sel) doCopy();
      else doPaste();
    }, true);
  }
}

/** 从客户端读取 .bash_history（经 AI 整理常用命令） */
async function loadCmdHistory() {
  loadingHistory.value = true;
  try {
    const agentId = panesForRender.value[0]?.agentId || activeTab.value?.panes[0];
    const { execCommand } = await import('@/api/agent');
    const agent = panesForRender.value[0]?.agentId;
    if (!agent) {
      MessagePlugin.warning('请先选择主机');
      return;
    }
    // 读取 root 或其他用户的 history，按路径探测
    const r = await execCommand(
      agent,
      'for f in /root/.bash_history /home/*/.bash_history; do [ -r "$f" ] && echo "### $f" && tail -100 "$f"; done 2>/dev/null',
      25,
    );
    const lines = (r.stdout || '').split('\n').filter((l) => l && !l.startsWith('###') && !l.startsWith('#'));
    const uniq: string[] = [];
    const seen = new Set<string>();
    for (let i = lines.length - 1; i >= 0 && uniq.length < 60; i--) {
      const c = lines[i].trim();
      if (c.length < 2 || seen.has(c)) continue;
      seen.add(c);
      uniq.push(c);
    }
    if (uniq.length) {
      cmdHistory.value = [...uniq, ...cmdHistory.value].slice(0, 200);
      MessagePlugin.success(`读取到 ${uniq.length} 条历史命令`);
    } else {
      MessagePlugin.info('未读取到历史命令（权限不足或无记录）');
    }
  } catch (e: any) {
    MessagePlugin.error(e.message || '读取失败');
  } finally {
    loadingHistory.value = false;
  }
}

/* ===== 资源监控侧栏 ===== */
const currentPaneAgent = computed(() => {
  const list = panesForRender.value;
  if (!list.length) return '';
  // 优先取聚焦窗格
  const focused = list.find((p) => p.id === focusedPaneId.value);
  return (focused || list[0])?.agentId || '';
});

function pct(v: any) { return v == null ? '-' : Number(v).toFixed(1); }
function num(v: any) { return v == null ? '-' : Number(v).toFixed(2); }
function barColor(v: any) {
  const n = Number(v) || 0;
  if (n >= 90) return '#e34d59';
  if (n >= 70) return '#ed7b2f';
  return '#0594fa';
}
function fmtKB(kb: any) {
  const n = Number(kb) || 0;
  if (n > 1024 * 1024) return (n / 1024 / 1024).toFixed(1) + ' GB';
  if (n > 1024) return (n / 1024).toFixed(0) + ' MB';
  return n.toFixed(0) + ' KB';
}
function fmtRate(bps: any) {
  const n = Number(bps) || 0;
  if (n > 1024 * 1024) return (n / 1024 / 1024).toFixed(1) + 'M/s';
  if (n > 1024) return (n / 1024).toFixed(0) + 'K/s';
  return n.toFixed(0) + 'B/s';
}
function uptime(sec: any) {
  const s0 = Number(sec) || 0;
  const d = Math.floor(s0 / 86400), h = Math.floor((s0 % 86400) / 3600), m = Math.floor((s0 % 3600) / 60);
  if (d) return `${d}天${h}时`;
  if (h) return `${h}时${m}分`;
  return `${m}分`;
}
function fmtSize(n: any) {
  const v = Number(n) || 0;
  if (v > 1024 * 1024 * 1024) return (v / 1024 / 1024 / 1024).toFixed(1) + 'G';
  if (v > 1024 * 1024) return (v / 1024 / 1024).toFixed(1) + 'M';
  if (v > 1024) return (v / 1024).toFixed(0) + 'K';
  return v + 'B';
}

async function fetchMonitor() {
  const agent = currentPaneAgent.value;
  if (!agent) return;
  monLoading.value = true;
  try {
    const { getAgent } = await import('@/api/agent');
    const a = await getAgent(agent);
    const m = a.last_metrics || {};
    monData.value = {
      ...m,
      net_rx: a.net_rx_rate || 0,
      net_tx: a.net_tx_rate || 0,
      net_conns: a.net_conns || 0,
    };
  } catch {
    /* 静默 */
  } finally {
    monLoading.value = false;
  }
}

function toggleMonitor() {
  showMonitor.value = !showMonitor.value;
  if (showMonitor.value) {
    // 启动网络历史采样（1s 间隔）
    if (netHistTimer) window.clearInterval(netHistTimer);
    netHist.value = [];
    netHistTimer = window.setInterval(pushNetHist, 1000);
    // 拉取监控数据
    fetchMonitor();
    if (monTimer) window.clearInterval(monTimer);
    monTimer = window.setInterval(fetchMonitor, 2000);
  } else {
    if (netHistTimer) { window.clearInterval(netHistTimer); netHistTimer = undefined; }
    if (monTimer) { window.clearInterval(monTimer); monTimer = undefined; }
  }
}

/** 切换标签/主机时，让监控与传输跟随当前 pane */
/** 切换标签后 fit + focus 当前 tab 的 pane（DOM 常驻，只需重新计算尺寸） */
function rebuildActiveTerminals() {
  nextTick(() => {
    for (const pane of panesForRender.value) {
      try { pane.fit?.fit(); pane.term?.focus(); } catch { /* ignore */ }
    }
  });
}

function refreshSidePanels() {
  if (showMonitor.value) fetchMonitor();
  if (showTransfer.value) {
    const pane = panesForRender.value[0];
    transferPath.value = pane?.cwd || '/';
    tLoad();
  }
}

/* ===== 文件传输侧栏 ===== */
/** 图标模式：按扩展名取图标 */
function tfIcon(f: any): string {
  const ext = (f.name.split('.').pop() || '').toLowerCase();
  const map: Record<string, string> = {
    md: 'file-word', txt: 'file', log: 'file',
    js: 'file-code', jsx: 'file-code', ts: 'file-code', tsx: 'file-code',
    py: 'file-code', go: 'file-code', c: 'file-code', h: 'file-code',
    cpp: 'file-code', java: 'file-code', sh: 'file-code', bash: 'file-code',
    json: 'file-code', yaml: 'file-code', yml: 'file-code', xml: 'file-code', toml: 'file-code',
    html: 'file-code', css: 'file-code', scss: 'file-code', less: 'file-code', vue: 'file-code',
    png: 'image', jpg: 'image', jpeg: 'image', gif: 'image', svg: 'image', webp: 'image',
    zip: 'file-zip', tar: 'file-zip', gz: 'file-zip', '7z': 'file-zip', rar: 'file-zip',
    pdf: 'file-pdf', doc: 'file-word', docx: 'file-word',
    xls: 'file-excel', xlsx: 'file-excel', csv: 'file-excel',
    mp4: 'video', mkv: 'video', mp3: 'sound', wav: 'sound', flac: 'sound',
    service: 'setting', conf: 'setting', ini: 'setting', env: 'setting',
    dockerfile: 'layers', 'docker-compose': 'layers',
  };
  return map[ext] || 'file';
}

async function tLoad(path?: string) {
  const agent = currentPaneAgent.value;
  if (!agent) {
    MessagePlugin.warning('请先选择主机');
    return;
  }
  if (path !== undefined) transferPath.value = path;
  transferLoading.value = true;
  try {
    const { listFiles } = await import('@/api/agent');
    const r = await listFiles(agent, transferPath.value);
    transferFiles.value = [...r.files].sort(
      (a, b) => Number(b.is_dir) - Number(a.is_dir) || a.name.localeCompare(b.name),
    );
  } catch (e: any) {
    MessagePlugin.error(e.message || '读取目录失败');
  } finally {
    transferLoading.value = false;
  }
}

function toggleTransfer() {
  showTransfer.value = !showTransfer.value;
  if (showTransfer.value) {
    // 初始路径跟随当前聚焦 pane 的工作目录（pane 独立跟踪）
    const pane = panesForRender.value.find((p: Pane) => p.id === focusedPaneId.value) || panesForRender.value[0];
    transferPath.value = pane?.cwd || terminalCwd.value || '/';
    tLoad();
  }
}

/** 同步终端当前工作目录 */
function syncCwd() {
  const pane = panesForRender.value.find((p: Pane) => p.id === focusedPaneId.value) || panesForRender.value[0];
  transferPath.value = pane?.cwd || terminalCwd.value || '/';
  tLoad();
}

function setTransferView(m: 'list' | 'icon') {
  transferView.value = m;
  localStorage.setItem('ctc_tf_view', m);
}

function tEnter(f: any) {
  if (f.is_dir) tLoad(f.path);
}

function tGoUp() {
  const segs = transferPath.value.split('/').filter(Boolean);
  segs.pop();
  tLoad('/' + segs.join('/'));
}

function tJoin(dir: string, name: string) {
  return dir === '/' ? '/' + name : dir.replace(/\/$/, '') + '/' + name;
}

async function tDownload(f: any) {
  const agent = currentPaneAgent.value;
  try {
    const { readFile } = await import('@/api/agent');
    const r = await readFile(agent, f.path);
    const bytes = Uint8Array.from(atob(r.content_base64), (c) => c.charCodeAt(0));
    const blob = new Blob([bytes]);
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = f.name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 20000);
    MessagePlugin.success('已下载 ' + f.name);
  } catch (e: any) {
    MessagePlugin.error(e.message || '下载失败');
  }
}

/** 打开文件编辑器 */
async function tEditFile(f: any) {
  const agent = currentPaneAgent.value;
  if (!agent) {
    MessagePlugin.warning('请先选择主机');
    return;
  }
  tEditLoading.value = true;
  tShowEditor.value = true;
  tEditPath.value = f.path;
  tEditContent.value = '';
  tEditBinary.value = false;
  tEditTruncated.value = false;
  tEditMode.value = 'view';
  tEditMarkdown.value = /\.(md|markdown|mdx)$/i.test(f.name);
  try {
    const { readFileText } = await import('@/api/agent');
    const r = await readFileText(agent, f.path);
    if (r.binary) {
      tEditBinary.value = true;
      const { DialogPlugin } = await import('tdesign-vue-next');
      const dlg = DialogPlugin.confirm({
        header: '二进制文件',
        body: `${f.name} 是二进制文件，无法在线编辑。是否下载？`,
        confirmBtn: '下载', cancelBtn: '取消',
        onConfirm: async () => { dlg.destroy(); await tDownload(f); },
        onCancel: () => { dlg.destroy(); tShowEditor.value = false; },
      });
    } else {
      tEditContent.value = r.content;
      tOriginalContent.value = r.content;
      tEditTruncated.value = r.truncated;
      tShowDiff.value = false;
      await nextTick();
      await new Promise((res) => setTimeout(res, 120));
      await nextTick();
      await tInitEditor();
    }
  } catch (e: any) {
    MessagePlugin.error(e.message || '读取失败');
    tShowEditor.value = false;
  } finally {
    tEditLoading.value = false;
  }
}

/** 初始化 CodeMirror 编辑器（共享加载器，保证 @codemirror/state 单例） */
async function tInitEditor() {
  const el = tEditorEl.value;
  if (!el) return;
  tDestroyEditor();
  try {
    const exts: any[] = [
      lineNumbers(),
      highlightActiveLineGutter(),
      bracketMatching(),
      history(),
      syntaxHighlighting(defaultHighlightStyle, { fallback: true }),
      keymap.of([...defaultKeymap, ...historyKeymap] as any),
      oneDark,
      EditorView.updateListener.of((u: any) => {
        if (u.docChanged) tEditContent.value = u.state.doc.toString();
      }),
    ];
    if (tEditMarkdown.value) exts.push(markdown());
    if (tEditMode.value === 'view') exts.push(EditorState.readOnly.of(true), EditorView.editable.of(false));
    const state = EditorState.create({ doc: tEditContent.value, extensions: exts });
    tCmView = new EditorView({ state, parent: el });
  } catch (e) {
    console.error('CodeMirror init error:', e);
    MessagePlugin.error('编辑器加载失败：' + (e as Error).message);
  }
}

function tDestroyEditor() {
  if (tCmView) { tCmView.destroy(); tCmView = null; }
}

/** 编辑器内查找下一个匹配 */
function tFindNext() {
  const q = tSearchQuery.value;
  if (!q || !tCmView) return;
  const cur = tCmView.state.selection.main.head;
  const doc = tCmView.state.doc.toString();
  let idx = doc.indexOf(q, cur);
  if (idx < 0) idx = doc.indexOf(q); // 回绕到开头
  if (idx < 0) { MessagePlugin.info('未找到: ' + q); return; }
  tCmView.dispatch({
    selection: { anchor: idx, head: idx + q.length },
    effects: EditorView.scrollIntoView(idx, { y: 'center' }),
  });
  tCmView.focus();
}

/** 保存文件 */
async function tSaveFile() {
  const agent = currentPaneAgent.value;
  if (!agent || !tEditPath.value) return;
  tEditSaving.value = true;
  try {
    const { saveFileText } = await import('@/api/agent');
    await saveFileText(agent, tEditPath.value, tEditContent.value);
    MessagePlugin.success('已保存');
    tShowEditor.value = false;
    tDestroyEditor();
  } catch (e: any) {
    MessagePlugin.error(e.message || '保存失败');
  } finally {
    tEditSaving.value = false;
  }
}

/** 删除文件 */
async function tDeleteFile(f: any) {
  const agent = currentPaneAgent.value;
  if (!agent) return;
  try {
    const { fileAction } = await import('@/api/agent');
    await fileAction(agent, f.path, 'delete');
    MessagePlugin.success('已删除 ' + f.name);
    tLoad();
  } catch (e: any) {
    MessagePlugin.error(e.message || '删除失败');
  }
}

function tUpload() {
  const el = document.querySelector('.transfer-panel input[type=file]') as HTMLInputElement | null;
  el?.click();
}

async function tOnPick(e: Event) {
  const input = e.target as HTMLInputElement;
  const files = Array.from(input.files || []);
  if (!files.length) return;
  const agent = currentPaneAgent.value;
  transferUploading.value = true;
  let done = 0;
  try {
    const { writeFile } = await import('@/api/agent');
    for (const file of files) {
      const buf = await file.arrayBuffer();
      const bytes = new Uint8Array(buf);
      let bin = '';
      const STEP = 8192;
      for (let i = 0; i < bytes.length; i += STEP) {
        bin += String.fromCharCode.apply(null, Array.from(bytes.subarray(i, i + STEP)) as any);
      }
      await writeFile(agent, tJoin(transferPath.value, file.name), btoa(bin));
      done++;
      uploadProgress.value = Math.round((done / files.length) * 100);
    }
    MessagePlugin.success(`已上传 ${done} 个文件`);
    tLoad();
  } catch (err: any) {
    MessagePlugin.error(err.message || '上传失败');
  } finally {
    transferUploading.value = false;
    uploadProgress.value = 0;
    input.value = '';
  }
}

let ro: ResizeObserver | null = null;

/** 统一聚焦 pane：聚焦时若文件传输已打开则跟随该 pane 的 cwd */
function focusPane(p: Pane | undefined) {
  if (!p) return;
  focusedPaneId.value = p.id;
  p.term?.focus();
  // 文件传输跟随当前聚焦 pane 的工作目录（无条件同步，切窗格即刷新）
  if (showTransfer.value) {
    const want = p.cwd || '/';
    if (want !== transferPath.value) {
      transferPath.value = want;
      tLoad();
    }
  }
  // 资源监控跟随当前聚焦 pane 的主机
  if (showMonitor.value) fetchMonitor();
}

/** 快捷键切换窗格：Ctrl+1/2/3/4 直接定位，Ctrl+Tab 循环 */
/** 当前聚焦 pane 在网格中的行列（四分屏 2x2） */
function paneGridPos(idx: number, total: number): { col: number; row: number; cols: number; rows: number } {
  if (total <= 1) return { col: 0, row: 0, cols: 1, rows: 1 };
  if (layout.value === '2v') return { col: idx, row: 0, cols: total, rows: 1 };
  if (layout.value === '2h') return { col: 0, row: idx, cols: 1, rows: total };
  // 四分屏：2 列 2 行，索引 0=左上 1=右上 2=左下 3=右下
  return { col: idx % 2, row: Math.floor(idx / 2), cols: 2, rows: 2 };
}

function onKeydown(e: KeyboardEvent) {
  const panes = panesForRender.value;
  if (panes.length <= 1) return;
  const curIdx = panes.findIndex((p) => p.id === focusedPaneId.value);
  const cur = curIdx < 0 ? 0 : curIdx;

  // Ctrl+反引号(`)：循环切换（Alt 被系统菜单栏拦截，此键不冲突）
  if (e.ctrlKey && !e.shiftKey && !e.altKey && (e.key === '`' || e.code === 'Backquote')) {
    e.preventDefault();
    focusPane(panes[(cur + 1) % panes.length]);
    return;
  }

  // Ctrl+Shift+反引号：反向循环
  if (e.ctrlKey && e.shiftKey && (e.key === '`' || e.code === 'Backquote' || e.key === '~')) {
    e.preventDefault();
    focusPane(panes[(cur - 1 + panes.length) % panes.length]);
    return;
  }

  // Ctrl+数字：直接定位到第 N 个窗格（xterm 聚焦时可达；浏览器标签页场景下可能被拦，优先用反引号/方向键）
  if (e.ctrlKey && !e.shiftKey && !e.altKey && e.key >= '1' && e.key <= '9') {
    e.preventDefault();
    const idx = parseInt(e.key, 10) - 1;
    if (idx < panes.length) focusPane(panes[idx]);
    return;
  }

  // Ctrl+Shift+方向键：按网格方向切换（四分屏上下左右都可，解决只能左右不能上下）
  if (e.ctrlKey && e.shiftKey && ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight'].includes(e.key)) {
    e.preventDefault();
    const pos = paneGridPos(cur, panes.length);
    let dc = 0, dr = 0;
    if (e.key === 'ArrowLeft') dc = -1;
    if (e.key === 'ArrowRight') dc = 1;
    if (e.key === 'ArrowUp') dr = -1;
    if (e.key === 'ArrowDown') dr = 1;
    // 目标行列（环绕）
    const nc = ((pos.col + dc) % pos.cols + pos.cols) % pos.cols;
    const nr = ((pos.row + dr) % pos.rows + pos.rows) % pos.rows;
    // 反查索引：布局决定映射
    let target: number;
    if (layout.value === '2v') target = nc;
    else if (layout.value === '2h') target = nr;
    else target = nr * 2 + nc; // 四分屏
    if (target < panes.length) focusPane(panes[target]);
    return;
  }

  // Ctrl+Shift+F：当前窗格全屏/还原（四分屏也支持）
  if (e.ctrlKey && e.shiftKey && (e.key === 'F' || e.key === 'f')) {
    e.preventDefault();
    const p = panes.find((x) => x.id === focusedPaneId.value) || panes[0];
    if (p) fullscreenPane(p);
    return;
  }
}

onMounted(async () => {
  window.addEventListener('click', hideTfCtxMenu);
  await loadAgents();
  fetchSuspended();
  // 默认自动打开一个终端标签
  if (tabs.value.length === 0) {
    const defaultAgent = (route.query.agent as string) || agents.value[0]?.value || '';
    if (defaultAgent) {
    addTab();
    // 单窗格模式默认聚焦第一个
    nextTick(() => {
      focusPane(panesForRender.value[0]);
    });
  }
  }
  window.addEventListener('resize', onResize);
  window.addEventListener('keydown', onKeydown);
  if (gridEl.value) {
    ro = new ResizeObserver(() => fitAll());
    ro.observe(gridEl.value);
    nextTick(() => makeResizable());
  }
});
onUnmounted(() => {
  window.removeEventListener('click', hideTfCtxMenu);
  window.removeEventListener('resize', onResize);
  window.removeEventListener('keydown', onKeydown);
  if (monTimer) window.clearInterval(monTimer);
  ro?.disconnect();
  panes.value.forEach((p) => {
    p.ws?.close();
    p.term?.dispose();
  });
});
</script>

<style scoped>
.term-page {
  display: flex;
  flex-direction: column;
  height: calc(100vh - 110px);
  background: var(--td-bg-color-page);
}
.term-page:fullscreen {
  height: 100vh;
  background: #141414;
  padding: 4px;
}
.term-page:fullscreen .term-toolbar {
  background: #1e1e1e;
  color: #d4d4d4;
}
.pane:fullscreen {
  background: #1e1e1e;
}
.term-toolbar {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 12px;
  background: var(--td-bg-color-container);
  border-radius: 6px;
  flex-wrap: wrap;
}
.hint { font-size: 12px; color: var(--td-text-color-secondary); }
.tab-bar { display: flex; gap: 2px; padding: 6px 12px 0; overflow-x: auto; }
.tab-item {
  display: flex; align-items: center; gap: 6px;
  padding: 6px 12px;
  background: var(--td-bg-color-container);
  border-radius: 6px 6px 0 0;
  cursor: pointer; font-size: 13px; opacity: 0.65; white-space: nowrap;
}
.tab-item.active { opacity: 1; background: #1e1e1e; color: #d4d4d4; }
.tab-close {
  position: relative;
  cursor: pointer;
  pointer-events: auto;
  z-index: 10;
  opacity: 0.7;
  flex-shrink: 0;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 18px;
  height: 18px;
  line-height: 1;
  font-size: 16px;
  border-radius: 3px;
  user-select: none;
}
.tab-close:hover { color: #e34d59; opacity: 1; background: rgba(255, 255, 255, 0.08); }
.term-grid {
  flex: 1;
  margin-right: 0; display: grid; gap: 4px; min-height: 0; padding: 4px;
  background: #141414; border-radius: 0 6px 6px 6px;
}
.layout-1 { grid-template-columns: 1fr; grid-template-rows: 1fr; }
.layout-2v { grid-template-columns: 1fr 1fr; grid-template-rows: 1fr; }
.layout-2h { grid-template-columns: 1fr; grid-template-rows: 1fr 1fr; }
.layout-4 { grid-template-columns: 1fr 1fr; grid-template-rows: 1fr 1fr; }

.pane {
  display: flex; flex-direction: column; min-width: 0; min-height: 0;
  background: #1e1e1e; border-radius: 4px; overflow: hidden;
}
.pane.focused {
  box-shadow: inset 3px 0 0 0 #00ff41; /* 仅左侧亮绿色，避免四边产生误解 */
}
/* 单窗口不显示选中边框 */
.work-area.inline-panels .pane.focused {
  box-shadow: none;
}
.pane-title {
  display: inline-block;
  max-width: 100%;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  vertical-align: middle;
}
.pane-bar {
  display: flex; justify-content: space-between; align-items: center;
  padding: 3px 8px; background: #252526; font-size: 11px; color: #9d9d9d; flex-shrink: 0;
}
.pane-state.st-open { color: #4ec9b0; }
.pane-state.st-connecting { color: #dcdcaa; }
.pane-state.st-closed { color: #f48771; }
.pane-ico { cursor: pointer; }
.pane-ico:hover { color: #fff; }
.pane-body { flex: 1; min-height: 0; padding: 2px; }
.warn-text { color: #ed7b2f; font-size: 13px; }

/* 挂起文字在深色 pane 上用白色 */
.pane-bar :deep(.suspend-white .t-checkbox__label) {
  color: #d4d4d4 !important;
}
.pane-bar :deep(.suspend-white) {
  color: #d4d4d4 !important;
}
.pane-bar :deep(.t-checkbox) {
  color: #d4d4d4 !important;
}

/* 命令历史侧栏 */
.cmd-history {
  position: fixed;
  right: 0;
  top: 96px;
  bottom: 0;
  width: 280px;
  background: var(--td-bg-color-container);
  border-left: 1px solid var(--td-component-stroke);
  display: flex;
  flex-direction: column;
  z-index: 100;
  box-shadow: -2px 0 8px rgba(0, 0, 0, 0.08);
}
.cmd-history-head {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 10px 12px;
  font-size: 13px;
  font-weight: 600;
  border-bottom: 1px solid var(--td-component-stroke);
}
.cmd-history-head .t-icon { cursor: pointer; }
.cmd-history-list {
  flex: 1;
  overflow-y: auto;
  padding: 4px 0;
}
.cmd-history-item {
  padding: 6px 12px;
  font-family: Menlo, Monaco, monospace;
  font-size: 12px;
  color: var(--td-text-color-secondary);
  cursor: pointer;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  border-left: 2px solid transparent;
}
.cmd-history-item:hover {
  background: var(--td-bg-color-container-hover);
  color: var(--td-brand-color);
  border-left-color: var(--td-brand-color);
}
.cmd-empty {
  padding: 40px 12px;
  text-align: center;
  font-size: 12px;
  color: var(--td-text-color-placeholder);
}
.cmd-history-foot {
  padding: 8px 12px;
  border-top: 1px solid var(--td-component-stroke);
}

/* ===== 侧栏（监控/传输）===== */
.tsp{position:relative;display:flex;flex-direction:column;background:var(--td-bg-color-container);border-left:1px solid var(--td-component-stroke);z-index:100}
/* 多窗口：恢复浮层定位 */
.work-area:not(.inline-panels) .tsp{position:fixed;right:0;top:96px;bottom:0;width:300px;box-shadow:-2px 0 8px rgba(0,0,0,0.08)}
/* 单屏嵌入：监控在右、文件在下（覆盖 position:fixed，强制嵌入布局） */
.work-area.inline-panels .tsp.monitor-panel,
.work-area.inline-panels .monitor-panel.inline-right{position:static !important;width:260px !important;flex-shrink:0;border-left:1px solid var(--td-component-stroke);align-self:stretch;max-height:none !important;box-shadow:none !important;top:auto !important;bottom:auto !important;right:auto !important;left:auto !important}
.work-area.inline-panels .tsp.transfer-panel{width:100% !important;height:320px;max-height:320px;border-left:none;border-top:1px solid var(--td-component-stroke);flex-shrink:0;flex-basis:100%;order:999}
.work-area.inline-panels .term-grid {
  flex: 1;
  min-width: 0;
  min-height: 0;
}
  background: var(--td-bg-color-container);
  border-left: 1px solid var(--td-component-stroke);
  display: flex;
  flex-direction: column;
  z-index: 100;
  box-shadow: -2px 0 8px rgba(0, 0, 0, 0.08);
.side-head {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 10px 12px;
  font-size: 13px;
  font-weight: 600;
  border-bottom: 1px solid var(--td-component-stroke);
}
.side-head .t-icon { cursor: pointer; }
.side-toolbar {
  display: flex;
  gap: 8px;
  padding: 8px 12px;
  border-bottom: 1px solid var(--td-component-stroke);
}
.side-body {
  flex: 1;
  overflow-y: auto;
  padding: 10px 12px;
}

/* 监控面板 */
.mon-item { margin-bottom: 14px; }
.mon-label {
  display: flex;
  justify-content: space-between;
  font-size: 12px;
  color: var(--td-text-color-secondary);
  margin-bottom: 6px;
}
.mon-label span { font-weight: 600; color: var(--td-text-color-primary); }
.mon-sub { font-size: 11px; color: var(--td-text-color-placeholder); margin-top: 2px; }
.mon-kv {
  display: flex;
  justify-content: space-between;
  padding: 6px 0;
  font-size: 12px;
  border-bottom: 1px dashed var(--td-component-stroke);
}
.mon-kv span { color: var(--td-text-color-secondary); }
.mon-kv b { color: var(--td-text-color-primary); font-weight: 500; }

/* 传输面板 */
.tf-item {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 8px;
  border-radius: 4px;
  cursor: pointer;
  font-size: 12px;
}
.tf-item:hover { background: var(--td-bg-color-container-hover); }
.tf-name {
  flex: 1;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.tf-size { color: var(--td-text-color-placeholder); font-size: 11px; }
.tf-dir { color: #f5b041; }
.tf-file { color: #5d9cec; }
.tf-dl {
  opacity: 0;
  cursor: pointer;
  color: var(--td-text-color-secondary);
}
.tf-item:hover .tf-dl { opacity: 1; }
.tf-empty {
  text-align: center;
  padding: 40px 0;
  color: var(--td-text-color-placeholder);
  font-size: 12px;
}

/* ===== 内嵌面板（单窗口）===== */
.work-area {
  flex: 1;
  display: flex;
  min-height: 0;
  overflow: hidden;
}
.work-area.inline-panels {
  gap: 4px;
  display: grid !important;
  grid-template-columns: 1fr;
  grid-template-rows: 1fr;
  grid-template-areas: "term";
}
/* 有监控时加右列 */
.work-area.inline-panels:has(.monitor-panel) {
  grid-template-columns: 1fr 260px;
  grid-template-areas: "term mon";
}
/* 有文件面板时加下行，文件面板占满整行 */
.work-area.inline-panels:has(.transfer-panel) {
  grid-template-rows: 1fr auto;
  grid-template-areas: "term" "files";
}
.work-area.inline-panels:has(.monitor-panel):has(.transfer-panel) {
  grid-template-columns: 1fr 260px;
  grid-template-rows: 1fr auto;
  grid-template-areas: "term mon" "files files";
}
.work-area.inline-panels .term-grid {
  grid-area: term;
  min-width: 0;
  min-height: 200px;
}
.work-area.inline-panels .tsp.monitor-panel,
.work-area.inline-panels .monitor-panel.inline-right {
  grid-area: mon;
  width: 260px;
  overflow-y: auto;
}
.work-area.inline-panels .tsp.transfer-panel,
.work-area.inline-panels .transfer-panel.inline-bottom {
  grid-area: files;
}
.work-area.inline-panels .term-grid {
  grid-column: 1;
  grid-row: 1;
  min-width: 0;
  min-height: 200px;
}
/* 单窗口：监控在右（宽度与 grid 列 260px 一致） */
.work-area.inline-panels .monitor-panel.inline-right {
  position: static;
  width: 260px;
  border-left: 1px solid var(--td-component-stroke);
  box-shadow: none;
  top: auto;
  bottom: auto;
  max-height: none;
}
.work-area.inline-panels .monitor-panel.inline-right .side-body {
  display: flex;
  flex-direction: column;
  padding: 8px 12px;
}
/* 文件传输：单窗口时显示在终端下方整行 */
.transfer-panel.inline-bottom {
  position: static;
  width: 100%;
  flex-basis: 100%;
  border-left: none;
  border-top: 1px solid var(--td-component-stroke);
  box-shadow: none;
  height: 340px;
  max-height: 340px;
  order: 999;
}
.transfer-panel.inline-bottom .side-body {
  padding: 4px 8px;
}

/* 多窗口：恢复浮层 */
.work-area:not(.inline-panels) .transfer-panel,
.work-area:not(.inline-panels) .monitor-panel {
  position: fixed;
  right: 0;
  top: 96px;
  bottom: 0;
  width: 300px;
  box-shadow: -2px 0 8px rgba(0, 0, 0, 0.08);
}

/* 快捷键帮助 */
.shortcut-list { display: flex; flex-direction: column; gap: 10px; }
.shortcut-row { display: flex; align-items: center; gap: 6px; font-size: 13px; }
.shortcut-row .keys { min-width: 170px; display: inline-flex; gap: 3px; align-items: center; }
.shortcut-row span:last-child { color: var(--td-text-color-secondary); }
.shortcut-row kbd {
  padding: 2px 6px; border: 1px solid var(--td-component-stroke);
  border-radius: 4px; background: var(--td-bg-color-page);
  font-family: monospace; font-size: 12px;
}
.shortcut-hint { margin-top: 6px; padding-top: 10px; border-top: 1px dashed var(--td-component-stroke); font-size: 12px; color: var(--td-text-color-secondary); }

/* 文件编辑对话框 */
.editor-wrap {
  border: 1px solid var(--td-component-stroke);
  border-radius: 6px;
  overflow: hidden;
  height: 420px;
  max-height: 60vh;
}
.editor-wrap .cm-editor { height: 100%; }
.editor-wrap .cm-scroller { overflow: auto; }
.editor-toolbar {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 8px 0;
  font-size: 13px;
  color: var(--td-text-color-secondary);
}
.editor-info {
  flex: 1;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.editor-binary {
  padding: 40px;
  text-align: center;
  color: var(--td-text-color-secondary);
}
.editor-truncated {
  padding: 8px 0;
  color: var(--td-warning-color);
  font-size: 13px;
}

/* 文件传输右键菜单 */
.tf-ctx-menu {
  position: fixed;
  background: var(--td-bg-color-container);
  border: 1px solid var(--td-component-stroke);
  border-radius: 6px;
  box-shadow: 0 4px 16px rgba(0,0,0,0.15);
  padding: 4px 0;
  min-width: 140px;
  z-index: 9999;
}
.tf-ctx-item {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 14px;
  font-size: 13px;
  color: var(--td-text-color-primary);
  cursor: pointer;
  transition: background 0.15s;
}
.tf-ctx-item:hover { background: var(--td-bg-color-page); }
.tf-ctx-item.danger { color: var(--td-error-color); }

/* 文件传输图标模式 */
.tf-icon-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(80px, 1fr));
  gap: 4px;
  padding: 4px;
}
.tf-icon-item {
  display: flex;
  flex-direction: column;
  align-items: center;
  padding: 10px 4px 6px;
  border-radius: 6px;
  cursor: pointer;
  border: 1px solid transparent;
  transition: all 0.15s;
  user-select: none;
}
.tf-icon-item:hover {
  background: var(--td-bg-color-container-hover);
  border-color: var(--td-component-stroke);
}
.tf-icon-item.selected {
  background: var(--td-brand-color-1);
  border-color: var(--td-brand-color);
}
.tf-icon-name {
  font-size: 11px;
  text-align: center;
  word-break: break-all;
  display: -webkit-box;
  -webkit-line-clamp: 2;
  -webkit-box-orient: vertical;
  overflow: hidden;
  max-width: 100%;
  margin-top: 4px;
  color: var(--td-text-color-primary);
}
.tf-icon-size {
  font-size: 10px;
  color: var(--td-text-color-placeholder);
  margin-top: 2px;
}
.side-body.icon-mode {
  padding: 4px;
}

/* ===== 仪表盘 ===== */
/* 动态仪表盘 */
.sys-info { padding-bottom: 4px; border-bottom: 1px solid var(--td-component-stroke); }
.sys-hostname { font-size: 14px; font-weight: 600; margin-bottom: 4px; }
.sys-meta { display: flex; flex-wrap: wrap; gap: 4px; }
.sys-tag { font-size: 11px; padding: 1px 6px; border-radius: 4px; background: var(--td-bg-color-page); color: var(--td-text-color-secondary); }
.sys-tag.ip { font-family: monospace; }
.dash-val { float: right; font-weight: 400; color: var(--td-text-color-secondary); font-size: 12px; }
.bar-wrap { position: relative; height: 8px; background: var(--td-bg-color-page); border-radius: 4px; overflow: hidden; margin: 6px 0 4px; }
.bar-fill { position: absolute; left: 0; top: 0; bottom: 0; border-radius: 4px; transition: width 0.5s ease; }
.bar-wrap.mem { display: flex; }
.bar-fill.used { background: #3491fa; }
.bar-fill.cached { background: #00b42a; }
.mem-legend { display: flex; flex-wrap: wrap; gap: 8px; font-size: 11px; color: var(--td-text-color-secondary); margin-bottom: 4px; }
.mem-legend .dot { display: inline-block; width: 8px; height: 8px; border-radius: 2px; margin-right: 3px; }
.dot.used { background: #3491fa; }
.dot.cached { background: #00b42a; }
.dot.free { background: var(--td-bg-color-page); border: 1px solid var(--td-component-stroke); }
.net-now { display: flex; gap: 12px; margin-bottom: 6px; }
.spark-chart { margin: 6px 0; }
.spark-chart svg { width: 100%; height: 30px; background: var(--td-bg-color-page); border-radius: 4px; }
.spark-legend { display: flex; gap: 10px; font-size: 11px; color: var(--td-text-color-secondary); margin-top: 3px; }
.spark-legend .dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 2px; }
.dot.rx { background: #00b42a; }
.dot.tx { background: #3491fa; }
.spark-legend .total { margin-left: auto; }
.disk-io { display: flex; gap: 12px; font-size: 12px; color: var(--td-text-color-secondary); margin-top: 4px; }
.proc-grid { display: flex; gap: 12px; }
.proc-item { text-align: center; flex: 1; }
.proc-val { font-size: 18px; font-weight: 600; }
.proc-label { font-size: 11px; color: var(--td-text-color-secondary); }
.monitor-panel.inline-right .side-body,
.work-area.inline-panels .monitor-panel .side-body {
  overflow-y: auto;
  max-height: 100%;
}
.dash-body {
  padding: 8px 10px;
}
.dash-section {
  margin-bottom: 14px;
  padding: 10px;
  background: var(--td-bg-color-container-hover);
  border-radius: 8px;
}
.dash-title {
  font-size: 12px;
  font-weight: 600;
  color: var(--td-text-color-secondary);
  margin-bottom: 8px;
  text-transform: uppercase;
  letter-spacing: 0.5px;
}
.gauge-row {
  display: flex;
  gap: 8px;
  justify-content: space-around;
}
.gauge-item {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 2px;
}
.gauge-label {
  font-size: 11px;
  color: var(--td-text-color-secondary);
}
.gauge-val {
  font-size: 12px;
  font-weight: 600;
  color: var(--td-text-color-primary);
}
.net-row {
  display: flex;
  gap: 12px;
  margin-bottom: 6px;
}
.net-cell {
  flex: 1;
  text-align: center;
  padding: 6px;
  background: var(--td-bg-color-container);
  border-radius: 6px;
}
.net-label {
  font-size: 11px;
  color: var(--td-text-color-placeholder);
}
.net-val {
  font-size: 16px;
  font-weight: 600;
  font-family: Menlo, Monaco, monospace;
}
.net-val.rx { color: #4fc3f7; }
.net-val.tx { color: #4caf50; }
</style>
