/*
 * Copyright (C) 2013 InventIt Inc. All rights reserved.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <uuid/uuid.h>
#include <ev.h>
#include <servicesync/moat.h>

#define APP_CONFIG_USE_SYSLOG

#ifdef APP_CONFIG_USE_SYSLOG
#include <syslog.h>
#define APP_LOG_OPEN()							openlog("PvDemo", LOG_PID, LOG_USER)
#define APP_LOG_CLOSE()							closelog()
#define APP_LOG_PRINT(priority, format, ...)	syslog(priority, " %s:%s():L%d " format "\n", __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define APP_LOG_ERROR(format, ...)				APP_LOG_PRINT(LOG_ERR, format, ##__VA_ARGS__)
#define APP_LOG_DEBUG(format, ...)				APP_LOG_PRINT(LOG_DEBUG, format, ##__VA_ARGS__)
#else /* APP_CONFIG_USE_SYSLOG */
#define APP_LOG_OPEN()
#define APP_LOG_CLOSE()
#define APP_LOG_PRINT(type, tag, format, ...)	printf("[" type "]" tag " %s:%s():L%d " format "\n", __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define APP_LOG_ERROR(format, ...)				APP_LOG_PRINT("**ERROR**", "PvDemo", format, ##__VA_ARGS__)
#define APP_LOG_DEBUG(format, ...)				APP_LOG_PRINT("DEBUG", "PvDemo", format, ##__VA_ARGS__)
#endif /* APP_CONFIG_USE_SYSLOG */

#define PV_DATA_MAX_COUNT					(100)
#define COLUMN_DELIMITER_CHR				' '
#define ERROR_CHR							'L'
#define PV_DATA_FILE						"/tmp/sensordata.txt"
#define PV_DEMO_CONFIG_FILE					"/tmp/pvdemo.conf"
#define PV_DATA_MAX_LENGTH					(256)

/*! 太陽光パネルデモアプリケーションクラス。
 * 
 * 本クラスは、Rooster GX にシリアル接続された太陽光パネルから、
 * 電圧及び電流を取得し、SUNSYNC の Backend サーバーに送信するクラスである。
 * 
 * \see http://dev.yourinventit.com/references/moat-c-api-document
 * \see http://dev.yourinventit.com/references/moat-iot-model-descriptor
 */
typedef struct PvDemoApp_ PvDemoApp;
struct PvDemoApp_ {
	/*! モデルデータを管理及び操作するための MOAT インタフェースの実体。
	 * 
	 * \see http://dev.yourinventit.com/references/moat-c-api-document#Moat
	 */
	Moat Moat;

	/*! ジョブサービス識別子 (Job Service Identifier)。
	 *
	 * \see http://dev.yourinventit.com/guides/moat-iot/app-design-in-moat-iot [Job Service Identifier]
	 */
	sse_char *Urn;

	/*! サービス ID。
	 *
	 * [Prefix]:[URN]:[Service Name]:[Suffix] の形式。
	 *
	 * 以下はサービス ID の例。
	 * <pre>
	 * "urn:moat:9999d129-5ba5-4912-963e-0edecee52664:twitter-demo:save-data:1.0"
	 * </pre>
	 *
	 * \see 
	 */
	sse_char *ServiceId;

	/*! センサーデータ収集タイマー。
	 *
	 * 本タイマーの満了契機で、太陽光パネルのセンサーデータを取得する。
	 */
	ev_periodic CollectionTimer;

	/*! センサーデータアップロードタイマー。
	 *
	 * 本タイマーの満了契機で、DataCollector に蓄積された太陽光パネルのセンサーデータをアップロードする。
	 */
	ev_periodic UploadTimer;

	/*! 収集したセンサーデータを管理する MOAT オブジェクト。
	 *
	 * 収集された太陽光パネルのセンサーデータは、一旦本オブジェクトに保存される。
	 *
	 * \see http://dev.yourinventit.com/references/moat-c-api-document#MoatObject
	 */
	MoatObject *DataCollection;
};

/*! サービス ID を生成する。
 *
 * 与えられた \a in_urn 及び \a in_service_name より、サービス ID を生成する。
 * サービス ID は、呼び出し側で解放する必要がある。
 *
 * @param [in] in_urn URN。
 * @param [in] in_service_name サービス名。
 * @return サービス ID。
 */
static sse_char *
create_notification_id(sse_char *in_urn, sse_char *in_service_name)
{
	sse_char *prefix = "urn:moat:";
	sse_uint prefix_len;
	sse_char *suffix = ":1.0";
	sse_uint suffix_len;
	sse_uint urn_len;
	sse_uint service_len;
	sse_char *noti_id = NULL;
	sse_char *p;

	prefix_len = strlen(prefix);
	urn_len = strlen(in_urn);
	service_len = strlen(in_service_name);
	suffix_len = strlen(suffix);
	noti_id = malloc(prefix_len + urn_len + 1 + service_len + suffix_len + 1);
	if (noti_id == NULL) {
		return NULL;
	}
	p = noti_id;
	memcpy(p, prefix, prefix_len);
	p += prefix_len;
	memcpy(p, in_urn, urn_len);
	p += urn_len;
	*p = ':';
	p++;
	memcpy(p, in_service_name, service_len);
	p += service_len;
	memcpy(p, suffix, suffix_len);
	p += suffix_len;
	*p = '\0';
	return noti_id;
}

/*! 太陽光パネルのセンサーデータ行を読み込む。
 *
 * \a in_path で指定したファイルから太陽光パネルのセンサーデータ行を読み込む。
 *
 * @param [in] in_path センサーデータが書き込まれたファイルのパス。
 * @return !=NULL \a in_path から読み込んだセンサーデータ行. 呼び出し元で解放する必要がある。
 * @return ==NULL センサーデータの読み込みに失敗した. 失敗要因は以下のいずれか。
 * - \a in_path のオープンに失敗。
 * - \a in_path のファイルサイズの取得に失敗。
 */
static sse_char *
read_record(sse_char *in_path)
{
	sse_int fd = -1;
	struct stat sb;
	sse_int read_bytes;
	sse_char *record = NULL;
	sse_char *p;
	sse_uint remain;
	sse_int i;
	sse_int err;

	fd = open(in_path, O_RDONLY);
	if (fd == -1) {
		err = SSE_E_GENERIC;
		goto error_exit;
	}
	err = fstat(fd, &sb);
	if (err < 0) {
		err = SSE_E_GENERIC;
		goto error_exit;
	}
	record = malloc(sb.st_size + 1);
	if (record == NULL ) {
		goto error_exit;
	}
	memset(record, 0, sb.st_size + 1);
	p = record;
	remain = sb.st_size;
	while (1) {
		read_bytes = read(fd, (void *)p, remain);
		if (read_bytes < 0) {
			err = SSE_E_INVAL;
			break;
		}
		p += read_bytes;
		remain -= read_bytes;
		if (read_bytes < sb.st_size) {
			break;
		}
		if (remain <= 0) {
			break;
		}
	}
	for (i = 0; i < sb.st_size; i++) {
		if (record[i] == 0x0A || record[i] == 0x0D) {
			record[i] = '\0';
		}
	}
	close(fd);
	return record;

error_exit:
	if (record != NULL) {
		free(record);
	}
	if (fd > 0) {
		close(fd);
	}
	return NULL;
}

/*! センサーデータを MOAT オブジェクトに変換する。
 *
 * \a in_record がポイントするバッファに書き込まれているセンサーデータを解析し、MOAT オブジェクトに変換する。
 * センサーデータのフォーマットは、サン電子株式会社が提供する PC20 ファイルフォーマットに準ずる。
 *
 * @param [in] in_record センサーデータ行。
 * @param [in] in_len \a in_recordがポイントするバッファの長さ。
 * @return センサーデータを格納したMOATオブジェクト。
 */
static MoatObject *
create_sensing_data(sse_char *in_record, sse_uint in_len)
{
	MoatObject *sensing_data = NULL;
	sse_char data[PV_DATA_MAX_LENGTH];
	struct timeval tv;
	sse_uint64 timestamp;
	sse_int err;
	sse_char *da;
	sse_uint da_len;
	sse_char *value;
	sse_uint value_len;
	sse_float v;
	sse_char *unit;
	sse_uint unit_len;
	sse_char *p;
	sse_char *e;

	if (in_len >= sizeof(data)) {
		return NULL;
	}
	strncpy(data, in_record, in_len + 1);
	da = data;
	p = strchr(da, COLUMN_DELIMITER_CHR);
	if (p == NULL) {
		goto error_exit;
	}
	da_len = p - da;
	*p = '\0';
	p++;
	if (*p == '\0' || da_len == 0 || da_len >= in_len) {
		goto error_exit;
	}
	value = p;
	p = strchr(value, COLUMN_DELIMITER_CHR);
	if (p == NULL) {
		goto error_exit;
	}
	value_len = p - value;
	*p = '\0';
	p++;
	if (*p == '\0' || value_len == 0 || (da_len + 1 + value_len) >= in_len) {
		goto error_exit;
	}
	e = strchr(value, ERROR_CHR);
	if (e != NULL) {
		APP_LOG_ERROR("data error. [%s]", in_record);
		goto error_exit;
	}
	unit = p;
	unit_len = strlen(unit);
	if (unit_len == 0) {
		goto error_exit;
	}
	v = strtod(value, NULL);

	err = gettimeofday(&tv, NULL);
	if (err) {
		err = SSE_E_GENERIC;
		goto error_exit;
	}
	timestamp = (sse_uint64)tv.tv_sec * 1000 + (sse_uint64)tv.tv_usec / 1000;

	sensing_data = moat_object_new();
	if (sensing_data == NULL) {
		goto error_exit;
	}
	err = moat_object_add_int64_value(sensing_data, "timestamp", timestamp, sse_true);
	if (err) {
		goto error_exit;
	}
	err = moat_object_add_string_value(sensing_data, "da", da, strlen(da), sse_true, sse_true);
	if (err) {
		goto error_exit;
	}

	err = moat_object_add_float_value(sensing_data, "value", v, sse_true);
	if (err) {
		goto error_exit;
	}
	err = moat_object_add_string_value(sensing_data, "unit", unit, unit_len, sse_true, sse_true);
	if (err) {
		goto error_exit;
	}
	return sensing_data;

error_exit:
	if (sensing_data != NULL) {
		moat_object_free(sensing_data);
	}
	return NULL;
}

/*! センサーデータをアップロードする。
 *
 * PvDemoApp#oDataCollection に蓄積されているセンサーデータを ServiceSync の Backend サーバーにアップロードする。
 *
 * @param [in] self 太陽光パネルデモアプリケーションクラスのインスタンス。
 * @return >0 リクエスト ID。 moat_send_notification が返す値。
 * @return SSE_OK センサーデータが存在しない。
 * @return SSE_INVAL \a MOAT オブジェクトが NULL である。
 */
static sse_int
pvdemoapp_upload_data(PvDemoApp *self)
{
	MoatObject *coll = self->DataCollection;
	sse_int len;
	sse_int req_id;

	if (coll == NULL) {
		return SSE_E_INVAL;
	}
	len = moat_object_get_length(coll);
	if (len == 0) {
		APP_LOG_DEBUG("no sensing data found.");
		return SSE_E_OK;
	}
	req_id = moat_send_notification(self->Moat, self->ServiceId, NULL, "SensingData", coll, NULL, NULL);
	moat_object_remove_all(coll);
	return req_id;
}

/*! アップロードタイマー満了時に呼び出されるコールバック関数。
 * 
 * 本コールバック関数が呼び出された契機で、 Backend サーバーにセンサーデータをアップロードする。
 * 
 * @param [in] loop メッセージループ。
 * @param [in] w 定期監視オブジェクト。
 * @param [in] revents イベント種別。
 * @see http://doc.dvgu.ru/devel/ev.html
 */
static void
pvdemoapp_upload_interval_proc(struct ev_loop *loop, ev_periodic *w, int revents)
{
	PvDemoApp *app = (PvDemoApp *)w->data;

	pvdemoapp_upload_data(app);
}

/*! センサーデータ収集タイマー満了時に呼び出されるコールバック関数。
 *
 * 本コールバック関数が呼び出された契機で、 センサーデータを収集する。
 * 
 * @param [in] loop メッセージループ。
 * @param [in] w 定期監視オブジェクト。
 * @param [in] revents イベント種別。
 * @see http://doc.dvgu.ru/devel/ev.html
 */
static void
pvdemoapp_sensing_interval_proc(struct ev_loop *loop, ev_periodic *w, int revents)
{
	PvDemoApp *app = (PvDemoApp *)w->data;
	sse_char *record = NULL;
	sse_uint len;
	MoatObject *sensing_data = NULL;
	MoatObject *coll;
	MoatObjectIterator *ite = NULL;
	sse_char *first_key;
	uuid_t uuid_hex;
	sse_char uuid_ascii[36 + 1];
	sse_uint data_count;
	sse_int err;

	coll = app->DataCollection;
	if (coll == NULL) {
		return;
	}
	data_count = moat_object_get_length(coll);
	if (data_count >= PV_DATA_MAX_COUNT) {
		ite = moat_object_create_iterator(coll);
		if (ite == NULL) {
			goto error_exit;
		}
		first_key = moat_object_iterator_get_next_key(ite);
		if (first_key != NULL) {
			moat_object_remove_value(coll, first_key);
		}
		moat_object_iterator_free(ite);
		ite = NULL;
	}
	record = read_record(PV_DATA_FILE);
	if (record == NULL) {
		APP_LOG_ERROR("failed to read a record from [%s].", PV_DATA_FILE);
		goto error_exit;
	}
	len = strlen(record);
	sensing_data = create_sensing_data(record, len);
	if (sensing_data == NULL) {
		APP_LOG_ERROR("failed to create a sensing data. record=[%s]", record);
		goto error_exit;
	}
	uuid_generate_random(uuid_hex);
	uuid_unparse(uuid_hex, uuid_ascii);
	err = moat_object_add_object_value(coll, uuid_ascii, sensing_data, sse_false, sse_true);
	if (err) {
		APP_LOG_ERROR("failed to add a sensing data into collection. record=[%s]", record);
		goto error_exit;
	}
	APP_LOG_DEBUG("** Added a sensing data:uuid=[%s], record=[%s]", uuid_ascii, record);
	if (!ev_is_active(&app->UploadTimer)) {
		pvdemoapp_upload_data(app);
	}
	free(record);
	return;

error_exit:
	if (sensing_data != NULL) {
		moat_object_free(sensing_data);
	}
	if (ite != NULL) {
		moat_object_iterator_free(ite);
	}
	if (record != NULL) {
		free(record);
	}
}

/*! 設定ファイルから太陽光パネルデモアプリケーションの設定情報を取得する。
 *
 * 設定ファイルから以下に示す情報を取得し、\a out_s_interval に収集間隔(秒)を、
 * \a out_u_interval にアップロード間隔(秒)を書き込む。
 * 
 * @param [out] センサー情報の収集間隔(秒)。
 * @param [out] センサー情報のアップロード間隔(秒)。
 */
static void
pvdemoapp_get_monitoring_config(sse_uint *out_s_interval, sse_uint *out_u_interval)
{
	sse_int s = 10;
	sse_int u = 30;
	sse_char *record = NULL;
	sse_char *p;

	record = read_record(PV_DEMO_CONFIG_FILE);
	if (record == NULL) {
		goto done;
	}
	p = record;
	p = strchr(p, COLUMN_DELIMITER_CHR);
	if (p == NULL) {
		goto done;
	}
	*p = '\0';
	p++;
	if (*p != '\0') {
		s = atoi(record);
		u = atoi(p);
	}
done:
	if (record != NULL) {
		free(record);
	}
	*out_s_interval = s;
	*out_u_interval = u;
	APP_LOG_DEBUG("sensing interval sec=[%d], upload interval sec=[%d]", s, u);
}

/*! 太陽光パネルデモアプリケーションを起動する。
 * 
 * \a self がポイントするデモアプリケーションクラスの実体を初期化する。
 * 
 * @param [in] self デモアプリケションの実体。
 * @return SSE_OK 正常に初期化が終了した。
 * @return SSE_E_NOMEM メモリ不足。
 */
static sse_int
pvdemoapp_start(PvDemoApp *self)
{
	struct ev_loop *loop;
	sse_char *noti_id = NULL;
	ModelMapper mapper;
	MoatObject *coll = NULL;
	sse_uint s_interval;
	sse_uint u_interval;
	sse_int err;

	noti_id = create_notification_id(self->Urn, "upload-sensing-data");
	if (noti_id == NULL) {
		APP_LOG_ERROR("failed to create service id.");
		goto error_exit;
	}
	coll = moat_object_new();
	if (coll == NULL) {
		APP_LOG_ERROR("failed to create data collection.");
		goto error_exit;
	}
	memset(&mapper, 0, sizeof(mapper));
	APP_LOG_DEBUG("DO register_model [SensingData]");
	err = moat_register_model(self->Moat, "SensingData", &mapper, self);
	if (err != SSE_E_OK) {
		APP_LOG_ERROR("failed to register model.");
		goto error_exit;
	}
	pvdemoapp_get_monitoring_config(&s_interval, &u_interval);
	if (s_interval > u_interval) {
		s_interval = u_interval;
	}
	self->ServiceId = noti_id;
	self->DataCollection = coll;
	loop = ev_default_loop(0);
	ev_periodic_init(&self->CollectionTimer, pvdemoapp_sensing_interval_proc, 0, (ev_tstamp)s_interval, 0);
	ev_periodic_start(loop, &self->CollectionTimer);
	self->CollectionTimer.data = self;
	if (s_interval != u_interval) {
		ev_periodic_init(&self->UploadTimer, pvdemoapp_upload_interval_proc, 0, (ev_tstamp)u_interval, 0);
		ev_periodic_start(loop, &self->UploadTimer);
		self->UploadTimer.data = self;
	}
	return SSE_E_OK;

error_exit:
	if (coll != NULL) {
		moat_object_free(coll);
	}
	if (noti_id != NULL) {
		free(noti_id);
	}
	return err;
}

/*! 太陽光パネルデモアプリケーションを停止する。
 * 
 * \a self がポイントするデモアプリケーションクラスを停止する。
 * 
 * @param [in] self デモアプリケションの実体。
 */
static void
pvdemoapp_stop(PvDemoApp *self)
{
	struct ev_loop *loop = ev_default_loop(0);

	if (ev_is_active(&self->CollectionTimer)) {
		ev_periodic_stop(loop, &self->CollectionTimer);
	}
	if (ev_is_active(&self->UploadTimer)) {
		ev_periodic_stop(loop, &self->UploadTimer);
	}
	moat_remove_model(self->Moat, "SensingData");
	if (self->DataCollection != NULL) {
		moat_object_free(self->DataCollection);
		self->DataCollection = NULL;
	}
	if (self->ServiceId != NULL) {
		free(self->ServiceId);
	}
}

/*! 太陽光パネルデモアプリケーションクラスのインスタンスを生成する。
 *
 * @param [in] in_moat MOAT オブジェクト。
 * @param [in] in_urn URN。
 * @return !=NULL 太陽光パネルデモアプリケーションのオブジェクト。
 * @return ==NULL オブジェクトの生成に失敗した。
 */
static PvDemoApp *
pvdemoapp_new(Moat in_moat, sse_char *in_urn)
{
	PvDemoApp *app = NULL;

	app = malloc(sizeof(PvDemoApp));
	if (app == NULL) {
		return NULL;
	}
	memset(app, 0, sizeof(PvDemoApp));
	app->Moat = in_moat;
	app->Urn = in_urn;
	return app;
}

/*! 太陽光パネルデモアプリケーションを解放する。
 *
 * @param [in] self 太陽光パネルデモアプリケーションのオブジェクト。
 */
static void
pvdemoapp_free(PvDemoApp *self)
{
	free(self);
}

/*! アプリケーションのエントリポイント。
 * 
 * ゲートウェイから呼び出されるアプリケーションのエントリポイント。
 * 
 * @param [in] argc アプリケーションの引数の数。
 * @param [in] argv アプリケーションの引数。in_argv[0] には、アプリケーションの URN が格納されている。
 * @return EXIT_SUCCESS 正常終了。
 * @return EXIT_FAILURE エラー終了。
 * @see http://dev.yourinventit.com/references/moat-c-api-document#MoatApp
 */
sse_int
moat_app_main(sse_int argc, sse_char *argv[])
{
	Moat moat;
	PvDemoApp *app;
	sse_int err;

	APP_LOG_OPEN();
	err = moat_init(argv[0], &moat);
	if (err != SSE_E_OK) {
		APP_LOG_ERROR("failed to initialize.");
		return EXIT_FAILURE;
	}
	app = pvdemoapp_new(moat, argv[0]);
	if (app == NULL) {
		moat_destroy(moat);
		APP_LOG_ERROR("failed to create a context.");
		return EXIT_FAILURE;
	}
	err = pvdemoapp_start(app);
	if (err == SSE_E_OK) {
	}
	moat_run(moat);
	pvdemoapp_stop(app);
	pvdemoapp_free(app);
	moat_destroy(moat);
	APP_LOG_CLOSE();
	return EXIT_SUCCESS;
}
