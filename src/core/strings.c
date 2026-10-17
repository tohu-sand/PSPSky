/*
 * English strings must use ASCII (0x20..0x7E and \n)
 * for the ltn0.pgf fallback font on non-Japanese PSPs.
 */

#include <stddef.h>

#include "pspsky/strings.h"

static pspsky_lang_t s_lang = PSPSKY_LANG_JA;

static const char k_unknown[] = "???";

static const char *const k_ja[PSPSKY_STR_COUNT] = {
    [PSPSKY_STR_ERR_OK] =
        "成功",
    [PSPSKY_STR_ERR_ARG] =
        "内部エラー(引数不正)",
    [PSPSKY_STR_ERR_NOMEM] =
        "メモリが不足しています",
    [PSPSKY_STR_ERR_TRANSPORT] =
        "通信に失敗しました。Wi-Fi接続を確認してください",
    [PSPSKY_STR_ERR_TLS] =
        "TLS証明書エラー。本体の日時設定を確認してください",
    [PSPSKY_STR_ERR_HTTP] =
        "サーバーエラーが発生しました",
    [PSPSKY_STR_ERR_AUTH] =
        "ログインに失敗しました。ハンドルとアプリパスワードを確認してください",
    [PSPSKY_STR_ERR_AUTH_EXPIRED] =
        "セッションの有効期限が切れました。再ログインしてください",
    [PSPSKY_STR_ERR_PARSE] =
        "サーバー応答の解析に失敗しました",
    [PSPSKY_STR_ERR_IO] =
        "ファイルの読み書きに失敗しました",
    [PSPSKY_STR_ERR_NET_INIT] =
        "ネットワーク初期化に失敗しました",
    [PSPSKY_STR_ERR_NET_CONNECT] =
        "アクセスポイントに接続できませんでした。接続設定1を確認してください",
    [PSPSKY_STR_ERR_CANCELLED] =
        "キャンセルされました",
    [PSPSKY_STR_ERR_RATELIMIT] =
        "リクエストが多すぎます。しばらく待ってから再試行してください",
    [PSPSKY_STR_ERR_UNKNOWN] =
        "不明なエラー",

    [PSPSKY_STR_TIME_JUST_NOW] =
        "今",
    [PSPSKY_STR_TIME_SEC_AGO] =
        "%d秒",
    [PSPSKY_STR_TIME_MIN_AGO] =
        "%d分",
    [PSPSKY_STR_TIME_HOUR_AGO] =
        "%d時間",
    [PSPSKY_STR_TIME_DAY_AGO] =
        "%d日",
    [PSPSKY_STR_TIME_MONTH_AGO] =
        "%dヶ月",

    [PSPSKY_STR_NET_CONNECTING_AP] =
        "アクセスポイントに接続中...",
    [PSPSKY_STR_NET_GETTING_IP] =
        "IPアドレスを取得中...",
    [PSPSKY_STR_NET_CONNECTED] =
        "接続しました",

    [PSPSKY_STR_UI_KEY_REFRESH] =
        "更新",
    [PSPSKY_STR_UI_KEY_MORE] =
        "さらに読み込む",
    [PSPSKY_STR_UI_KEY_POST] =
        "投稿",
    [PSPSKY_STR_UI_KEY_QUIT] =
        "終了",
    [PSPSKY_STR_UI_KEY_OK] =
        "OK",
    [PSPSKY_STR_UI_KEY_YES] =
        "はい",
    [PSPSKY_STR_UI_KEY_NO] =
        "いいえ",
    [PSPSKY_STR_UI_KEY_POST_IMAGE] =
        "画像を追加",
    [PSPSKY_STR_UI_KEY_REMOVE_IMAGE] =
        "最後を削除",
    [PSPSKY_STR_UI_KEY_CANCEL] =
        "キャンセル",
    [PSPSKY_STR_UI_KEY_SELECT] =
        "決定",
    [PSPSKY_STR_UI_KEY_POST_THIS] =
        "この画像を追加",
    [PSPSKY_STR_UI_KEY_RECHOOSE] =
        "選び直す",
    [PSPSKY_STR_UI_TAB_FOLLOWING] =
        "フォロー中",
    [PSPSKY_STR_UI_REPOSTED_BY] =
        "%sがリポスト",
    [PSPSKY_STR_UI_REPOSTED_BY_YOU] =
        "あなたのリポスト",
    [PSPSKY_STR_UI_REPLIED_TO] =
        "%sへの返信",
    [PSPSKY_STR_UI_REPLIED_TO_YOU] =
        "あなたへの返信",
    [PSPSKY_STR_UI_REPLIED_TO_POST] =
        "投稿への返信",
    [PSPSKY_STR_UI_REPLIED_TO_BLOCKED] =
        "ブロックされた投稿への返信",
    [PSPSKY_STR_UI_QUOTE_NOT_FOUND] =
        "削除された投稿",
    [PSPSKY_STR_UI_QUOTE_BLOCKED] =
        "ブロックされた投稿",
    [PSPSKY_STR_UI_QUOTE_DETACHED] =
        "投稿者が引用を解除しました",
    [PSPSKY_STR_UI_EMBED_MARK] =
        "画像",
    [PSPSKY_STR_UI_IMG_MARK_N] =
        "画像 %d枚",
    [PSPSKY_STR_UI_COMPOSE_IMAGES] =
        "添付画像 %d/%d枚",
    [PSPSKY_STR_UI_PREV_TITLE] =
        "画像プレビュー",
    [PSPSKY_STR_UI_PREV_NO_IMAGE] =
        "プレビューを表示できません",
    [PSPSKY_STR_UI_PREV_NO_IMAGE_SUB] =
        "(非対応の画像形式か、読み込みに失敗しました)",
    [PSPSKY_STR_UI_PREV_DIM_UNKNOWN] =
        "(寸法不明)",
    [PSPSKY_STR_UI_PICK_EMPTY] =
        "(ファイルがありません)",
    [PSPSKY_STR_UI_TL_EMPTY] =
        "表示できる投稿がありません",
    [PSPSKY_STR_UI_MSG_PATH_TOO_LONG] =
        "パスが長すぎます",
    [PSPSKY_STR_UI_MSG_DIR_OPEN_FAIL] =
        "フォルダを開けません",

    [PSPSKY_STR_CACERT_NOT_FOUND] =
        "cacert.pem が見つかりません",
    [PSPSKY_STR_CACERT_UNREADABLE] =
        "cacert.pem を読み込めません",
    [PSPSKY_STR_CA_DIAG_PREFIX] =
        "CA診断: ",
    [PSPSKY_STR_ST_NET_INIT] =
        "ネットワーク初期化中...",
    [PSPSKY_STR_ST_TRANSPORT_INIT] =
        "通信の準備中...",
    [PSPSKY_STR_ST_LOGGING_IN] =
        "ログイン中...",
    [PSPSKY_STR_ST_TIMELINE_LOADING] =
        "タイムライン取得中...",
    [PSPSKY_STR_ST_LOADING_MORE] =
        "さらに読み込み中...",
    [PSPSKY_STR_ST_FETCHING_IMAGES] =
        "画像を取得中 (%d/%d)...",
    [PSPSKY_STR_ST_POSTING] =
        "投稿中...",
    [PSPSKY_STR_ST_UPLOADING] =
        "画像をアップロード中 (%d/%d)...",
    [PSPSKY_STR_ST_PREVIEW_PREP] =
        "プレビューを準備中...",
    [PSPSKY_STR_OSK_HANDLE] =
        "Blueskyハンドル(例 user.bsky.social)",
    [PSPSKY_STR_OSK_PASSWORD] =
        "アプリパスワード(xxxx-xxxx-xxxx-xxxx)",
    [PSPSKY_STR_OSK_COMPOSE] =
        "投稿する内容",
    [PSPSKY_STR_OSK_ALT] =
        "画像の説明(ALTテキスト・省略可)",
    [PSPSKY_STR_MSG_HANDLE_EMPTY] =
        "ハンドルを入力してください",
    [PSPSKY_STR_MSG_HANDLE_TOO_LONG] =
        "ハンドルが長すぎます",
    [PSPSKY_STR_MSG_PASSWORD_EMPTY] =
        "アプリパスワードを入力してください",
    [PSPSKY_STR_MSG_PASSWORD_TOO_LONG] =
        "アプリパスワードが長すぎます",
    [PSPSKY_STR_MSG_CFG_SAVE_FAIL] =
        "設定を保存できませんでした。\n次回も入力が必要です",
    [PSPSKY_STR_CONFIRM_RETRY] =
        "もう一度試しますか？",
    [PSPSKY_STR_MSG_TIME_FAIL] =
        "時刻の取得に失敗しました",
    [PSPSKY_STR_MSG_POSTED] =
        "投稿しました",
    [PSPSKY_STR_MSG_IMG_UNSUPPORTED] =
        "対応していない画像形式です(JPEG/PNGのみ)",
    [PSPSKY_STR_MSG_IMG_TOO_LARGE] =
        "画像が大きすぎます(最大2,000,000バイト)",
    [PSPSKY_STR_MSG_IMG_READ_FAIL] =
        "画像を読み込めませんでした",
    [PSPSKY_STR_MSG_TEXT_TOO_LONG] =
        "300文字以内にしてください",
    [PSPSKY_STR_CONFIRM_QUIT] =
        "終了しますか？",
    [PSPSKY_STR_MSG_SESSION_EXPIRED_RELOGIN] =
        "セッションの有効期限が切れました。\n再ログインします",
    [PSPSKY_STR_CONFIRM_ABORT_QUIT] =
        "入力を中止しました。終了しますか？",

    [PSPSKY_STR_TRC_CACERT_READ_PRE] =
        "cacert.pem を読み込めません: ",
    [PSPSKY_STR_TRC_PATH_UNSET] =
        "(パス未設定)",
    [PSPSKY_STR_TRC_CERT_VERIFY] =
        "証明書検証失敗 (curl:60)",
    [PSPSKY_STR_TRC_TLS_HANDSHAKE] =
        "TLSハンドシェイクに失敗。電波状態を確認して再試行してください (curl:35)%s",
    [PSPSKY_STR_TRC_CONNECT_FAIL] =
        "接続できませんでした (curl:%d)",
    [PSPSKY_STR_TRC_COMM_FAIL] =
        "通信に失敗しました (curl:%d)",
    [PSPSKY_STR_TRC_RESP_TOO_LARGE] =
        "応答が大きすぎます",
};

static const char *const k_en[PSPSKY_STR_COUNT] = {
    [PSPSKY_STR_ERR_OK] =
        "Success",
    [PSPSKY_STR_ERR_ARG] =
        "Internal error (bad argument)",
    [PSPSKY_STR_ERR_NOMEM] =
        "Out of memory",
    [PSPSKY_STR_ERR_TRANSPORT] =
        "Connection failed. Check your Wi-Fi connection",
    [PSPSKY_STR_ERR_TLS] =
        "TLS certificate error. Check the system date and time",
    [PSPSKY_STR_ERR_HTTP] =
        "Server error",
    [PSPSKY_STR_ERR_AUTH] =
        "Login failed. Check your handle and app password",
    [PSPSKY_STR_ERR_AUTH_EXPIRED] =
        "Session expired. Please log in again",
    [PSPSKY_STR_ERR_PARSE] =
        "Failed to parse server response",
    [PSPSKY_STR_ERR_IO] =
        "File read/write error",
    [PSPSKY_STR_ERR_NET_INIT] =
        "Network initialization failed",
    [PSPSKY_STR_ERR_NET_CONNECT] =
        "Could not connect to the access point. Check network connection 1",
    [PSPSKY_STR_ERR_CANCELLED] =
        "Cancelled",
    [PSPSKY_STR_ERR_RATELIMIT] =
        "Too many requests. Please wait and try again",
    [PSPSKY_STR_ERR_UNKNOWN] =
        "Unknown error",

    [PSPSKY_STR_TIME_JUST_NOW] =
        "now",
    [PSPSKY_STR_TIME_SEC_AGO] =
        "%ds",
    [PSPSKY_STR_TIME_MIN_AGO] =
        "%dm",
    [PSPSKY_STR_TIME_HOUR_AGO] =
        "%dh",
    [PSPSKY_STR_TIME_DAY_AGO] =
        "%dd",
    [PSPSKY_STR_TIME_MONTH_AGO] =
        "%dmo",

    [PSPSKY_STR_NET_CONNECTING_AP] =
        "Connecting to access point...",
    [PSPSKY_STR_NET_GETTING_IP] =
        "Getting IP address...",
    [PSPSKY_STR_NET_CONNECTED] =
        "Connected",

    [PSPSKY_STR_UI_KEY_REFRESH] =
        "Refresh",
    [PSPSKY_STR_UI_KEY_MORE] =
        "Load more",
    [PSPSKY_STR_UI_KEY_POST] =
        "Post",
    [PSPSKY_STR_UI_KEY_QUIT] =
        "Quit",
    [PSPSKY_STR_UI_KEY_OK] =
        "OK",
    [PSPSKY_STR_UI_KEY_YES] =
        "Yes",
    [PSPSKY_STR_UI_KEY_NO] =
        "No",
    [PSPSKY_STR_UI_KEY_POST_IMAGE] =
        "Add image",
    [PSPSKY_STR_UI_KEY_REMOVE_IMAGE] =
        "Remove last",
    [PSPSKY_STR_UI_KEY_CANCEL] =
        "Cancel",
    [PSPSKY_STR_UI_KEY_SELECT] =
        "Select",
    [PSPSKY_STR_UI_KEY_POST_THIS] =
        "Attach image",
    [PSPSKY_STR_UI_KEY_RECHOOSE] =
        "Choose again",
    [PSPSKY_STR_UI_TAB_FOLLOWING] =
        "Following",
    [PSPSKY_STR_UI_REPOSTED_BY] =
        "Reposted by %s",
    [PSPSKY_STR_UI_REPOSTED_BY_YOU] =
        "Reposted by you",
    [PSPSKY_STR_UI_REPLIED_TO] =
        "Replied to %s",
    [PSPSKY_STR_UI_REPLIED_TO_YOU] =
        "Replied to you",
    [PSPSKY_STR_UI_REPLIED_TO_POST] =
        "Replied to a post",
    [PSPSKY_STR_UI_REPLIED_TO_BLOCKED] =
        "Replied to a blocked post",
    [PSPSKY_STR_UI_QUOTE_NOT_FOUND] =
        "Deleted post",
    [PSPSKY_STR_UI_QUOTE_BLOCKED] =
        "Blocked post",
    [PSPSKY_STR_UI_QUOTE_DETACHED] =
        "Removed by author",
    [PSPSKY_STR_UI_EMBED_MARK] =
        "Image",
    [PSPSKY_STR_UI_IMG_MARK_N] =
        "%d images",
    [PSPSKY_STR_UI_COMPOSE_IMAGES] =
        "Images %d/%d",
    [PSPSKY_STR_UI_PREV_TITLE] =
        "Image preview",
    [PSPSKY_STR_UI_PREV_NO_IMAGE] =
        "Cannot show preview",
    [PSPSKY_STR_UI_PREV_NO_IMAGE_SUB] =
        "(unsupported image format or read error)",
    [PSPSKY_STR_UI_PREV_DIM_UNKNOWN] =
        "(size unknown)",
    [PSPSKY_STR_UI_PICK_EMPTY] =
        "(no files)",
    [PSPSKY_STR_UI_TL_EMPTY] =
        "No posts to show",
    [PSPSKY_STR_UI_MSG_PATH_TOO_LONG] =
        "Path is too long",
    [PSPSKY_STR_UI_MSG_DIR_OPEN_FAIL] =
        "Cannot open folder",

    [PSPSKY_STR_CACERT_NOT_FOUND] =
        "cacert.pem not found",
    [PSPSKY_STR_CACERT_UNREADABLE] =
        "Cannot read cacert.pem",
    [PSPSKY_STR_CA_DIAG_PREFIX] =
        "CA diag: ",
    [PSPSKY_STR_ST_NET_INIT] =
        "Initializing network...",
    [PSPSKY_STR_ST_TRANSPORT_INIT] =
        "Preparing connection...",
    [PSPSKY_STR_ST_LOGGING_IN] =
        "Logging in...",
    [PSPSKY_STR_ST_TIMELINE_LOADING] =
        "Loading timeline...",
    [PSPSKY_STR_ST_LOADING_MORE] =
        "Loading more...",
    [PSPSKY_STR_ST_FETCHING_IMAGES] =
        "Fetching images (%d/%d)...",
    [PSPSKY_STR_ST_POSTING] =
        "Posting...",
    [PSPSKY_STR_ST_UPLOADING] =
        "Uploading image (%d/%d)...",
    [PSPSKY_STR_ST_PREVIEW_PREP] =
        "Preparing preview...",
    [PSPSKY_STR_OSK_HANDLE] =
        "Bluesky handle (e.g. user.bsky.social)",
    [PSPSKY_STR_OSK_PASSWORD] =
        "App password (xxxx-xxxx-xxxx-xxxx)",
    [PSPSKY_STR_OSK_COMPOSE] =
        "Post text",
    [PSPSKY_STR_OSK_ALT] =
        "Image description (ALT text, optional)",
    [PSPSKY_STR_MSG_HANDLE_EMPTY] =
        "Please enter your handle",
    [PSPSKY_STR_MSG_HANDLE_TOO_LONG] =
        "Handle is too long",
    [PSPSKY_STR_MSG_PASSWORD_EMPTY] =
        "Please enter your app password",
    [PSPSKY_STR_MSG_PASSWORD_TOO_LONG] =
        "App password is too long",
    [PSPSKY_STR_MSG_CFG_SAVE_FAIL] =
        "Could not save settings.\nYou will need to enter them again",
    [PSPSKY_STR_CONFIRM_RETRY] =
        "Try again?",
    [PSPSKY_STR_MSG_TIME_FAIL] =
        "Failed to get current time",
    [PSPSKY_STR_MSG_POSTED] =
        "Posted",
    [PSPSKY_STR_MSG_IMG_UNSUPPORTED] =
        "Unsupported image format (JPEG/PNG only)",
    [PSPSKY_STR_MSG_IMG_TOO_LARGE] =
        "Image is too large (max 2,000,000 bytes)",
    [PSPSKY_STR_MSG_IMG_READ_FAIL] =
        "Could not read image",
    [PSPSKY_STR_MSG_TEXT_TOO_LONG] =
        "Post must be 300 characters or less",
    [PSPSKY_STR_CONFIRM_QUIT] =
        "Quit PSPSky?",
    [PSPSKY_STR_MSG_SESSION_EXPIRED_RELOGIN] =
        "Session expired.\nLogging in again",
    [PSPSKY_STR_CONFIRM_ABORT_QUIT] =
        "Input cancelled. Quit PSPSky?",

    [PSPSKY_STR_TRC_CACERT_READ_PRE] =
        "Cannot read cacert.pem: ",
    [PSPSKY_STR_TRC_PATH_UNSET] =
        "(no path set)",
    [PSPSKY_STR_TRC_CERT_VERIFY] =
        "Cert verification failed (curl:60)",
    [PSPSKY_STR_TRC_TLS_HANDSHAKE] =
        "TLS handshake failed. Check signal and retry (curl:35)%s",
    [PSPSKY_STR_TRC_CONNECT_FAIL] =
        "Could not connect (curl:%d)",
    [PSPSKY_STR_TRC_COMM_FAIL] =
        "Communication failed (curl:%d)",
    [PSPSKY_STR_TRC_RESP_TOO_LARGE] =
        "Response too large",
};

void pspsky_lang_set(pspsky_lang_t lang)
{
    s_lang = (lang == PSPSKY_LANG_EN) ? PSPSKY_LANG_EN : PSPSKY_LANG_JA;
}

pspsky_lang_t pspsky_lang_get(void)
{
    return s_lang;
}

const char *pspsky_tr_lang(pspsky_lang_t lang, pspsky_str_id_t id)
{
    const char *const *tbl;
    const char *s;

    if ((int)id < 0 || (int)id >= (int)PSPSKY_STR_COUNT) {
        return k_unknown;
    }

    tbl = (lang == PSPSKY_LANG_EN) ? k_en : k_ja;
    s = tbl[(int)id];

    return (s != NULL) ? s : k_unknown;
}

const char *pspsky_tr(pspsky_str_id_t id)
{
    return pspsky_tr_lang(s_lang, id);
}
