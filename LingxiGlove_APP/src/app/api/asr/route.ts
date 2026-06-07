/**
 * ============================================================================
 *  /api/asr — 阿里云一句话识别 RESTful API 代理
 *
 *  职责：
 *   1. 接收前端 POST 请求（body = raw PCM 16-bit LE mono 16kHz）
 *   2. 获取阿里云 Token（支持两种方式，见下文）
 *   3. 转发至阿里云 NLS Gateway，返回标准化 JSON 响应
 *
 *  Token 获取策略（按优先级）：
 *   1. 若配置了 ALIYUN_ASR_TOKEN → 直接使用（临时 Token，手动管理）
 *   2. 若配置了 ALIYUN_ACCESS_KEY_ID + SECRET → 自动调用 CreateToken（推荐）
 *
 *  安全设计：
 *   - 所有凭证仅在服务端环境变量中，前端不接触任何密钥
 * ============================================================================
 */

import { NextRequest, NextResponse } from 'next/server';

/* eslint-disable */
const RPCClient = require('@alicloud/pop-core');
/* eslint-enable */

/** 阿里云一句话识别服务地址（上海） */
const ALIYUN_ASR_URL =
  'https://nls-gateway-cn-shanghai.aliyuncs.com/stream/v1/asr';

// ─── Token 缓存（自动模式用） ───────────────────────────────────────────────

interface TokenCache {
  token: string;
  /** 过期时间戳（秒），提前 5 分钟刷新 */
  expireTime: number;
}

let cachedToken: TokenCache | null = null;

/**
 * 获取有效 Token。
 *
 * 策略：
 *   1. 若 ALIYUN_ASR_TOKEN 已配置 → 直接返回（用户手动管理）
 *   2. 否则使用 AccessKey ID/Secret 自动获取并缓存
 */
async function getToken(): Promise<string> {
  // ─── 方式一：手动临时 Token ────────────────────────────────────────────────
  const manualToken = process.env.ALIYUN_ASR_TOKEN;
  if (manualToken && manualToken !== 'your_token_here') {
    return manualToken;
  }

  // ─── 方式二：AccessKey 自动获取 ──────────────────────────────────────────
  const now = Math.floor(Date.now() / 1000);
  // 提前 300 秒（5 分钟）刷新，避免请求时刚好过期
  if (cachedToken && cachedToken.expireTime - 300 > now) {
    return cachedToken.token;
  }

  const accessKeyId = process.env.ALIYUN_ACCESS_KEY_ID;
  const accessKeySecret = process.env.ALIYUN_ACCESS_KEY_SECRET;

  if (!accessKeyId || !accessKeySecret || accessKeyId === 'your_access_key_id_here') {
    throw new Error(
      '未配置有效凭证。请在 .env.local 中设置 ALIYUN_ASR_TOKEN（临时）或 ALIYUN_ACCESS_KEY_ID + ALIYUN_ACCESS_KEY_SECRET（自动）',
    );
  }

  const client = new RPCClient({
    accessKeyId,
    accessKeySecret,
    endpoint: 'https://nls-meta.cn-shanghai.aliyuncs.com',
    apiVersion: '2019-02-28',
  });

  const result = await client.request('CreateToken', {}, { method: 'POST' });

  if (!result?.Token?.Id) {
    throw new Error(`CreateToken 失败: ${JSON.stringify(result)}`);
  }

  cachedToken = {
    token: result.Token.Id,
    expireTime: result.Token.ExpireTime,
  };

  console.log(
    `[ASR Route] Token 已刷新，有效至 ${new Date(cachedToken.expireTime * 1000).toLocaleString()}`,
  );

  return cachedToken.token;
}

// ─── ASR 请求处理 ──────────────────────────────────────────────────────────

interface AliyunAsrResponse {
  task_id: string;
  result: string;
  status: number;
  message: string;
}

export async function POST(request: NextRequest) {
  const appkey = process.env.ALIYUN_ASR_APPKEY;

  if (!appkey || appkey === 'your_appkey_here') {
    return NextResponse.json(
      { error: 'ASR 服务未配置，请在 .env.local 中设置 ALIYUN_ASR_APPKEY' },
      { status: 503 },
    );
  }

  // 获取 Token（自动缓存 + 刷新）
  let token: string;
  try {
    token = await getToken();
  } catch (err) {
    console.error('[ASR Route] Token 获取失败:', err);
    return NextResponse.json(
      { error: `Token 获取失败: ${err instanceof Error ? err.message : String(err)}` },
      { status: 503 },
    );
  }

  // 读取前端发来的 PCM 二进制数据
  const pcmBuffer = await request.arrayBuffer();
  if (!pcmBuffer || pcmBuffer.byteLength === 0) {
    return NextResponse.json(
      { error: '请求体为空，需要 PCM 音频数据' },
      { status: 400 },
    );
  }

  // 拼接阿里云请求 URL
  const params = new URLSearchParams({
    appkey,
    format: 'pcm',
    sample_rate: '16000',
    enable_punctuation_prediction: 'true',
    enable_inverse_text_normalization: 'true',
    enable_voice_detection: 'true',
  });
  const url = `${ALIYUN_ASR_URL}?${params.toString()}`;

  const startTime = Date.now();

  try {
    const response = await fetch(url, {
      method: 'POST',
      headers: {
        'X-NLS-Token': token,
        'Content-Type': 'application/octet-stream',
        'Content-Length': String(pcmBuffer.byteLength),
      },
      body: pcmBuffer,
    });

    const latencyMs = Date.now() - startTime;

    if (!response.ok) {
      const text = await response.text();
      console.error('[ASR Route] Aliyun HTTP error:', response.status, text);
      return NextResponse.json(
        { error: `阿里云返回 HTTP ${response.status}`, detail: text },
        { status: 502 },
      );
    }

    const data: AliyunAsrResponse = await response.json();

    // Token 过期时清除缓存，下次请求自动刷新
    if (data.status === 40000001) {
      cachedToken = null;
      console.warn('[ASR Route] Token 已过期，已清除缓存');
      return NextResponse.json(
        { error: 'Token 已过期，请重试', aliyunStatus: data.status },
        { status: 401 },
      );
    }

    if (data.status !== 20000000) {
      console.error('[ASR Route] Aliyun ASR error:', data.status, data.message);
      return NextResponse.json(
        { error: data.message, aliyunStatus: data.status, taskId: data.task_id },
        { status: 422 },
      );
    }

    return NextResponse.json({
      text: data.result || '',
      latencyMs,
      taskId: data.task_id,
    });
  } catch (err) {
    const latencyMs = Date.now() - startTime;
    console.error('[ASR Route] fetch error:', err);
    return NextResponse.json(
      { error: `网络错误: ${err instanceof Error ? err.message : String(err)}`, latencyMs },
      { status: 500 },
    );
  }
}
