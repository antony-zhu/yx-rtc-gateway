#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../../../embedded/components/gw_app/src/gw/gw_gateway_protocol.c"

static char *s_published[64];
static size_t s_published_count;
static unsigned s_timer_calls, s_ota_submits, s_ota_starts, s_rollback_calls, s_reboots;
static esp_err_t s_submit_error, s_publish_error;
static gw_ota_upgrade_request_t s_submitted;

esp_err_t gw_business_mqtt_publish_gateway(const void *data,size_t len,gw_business_mqtt_gateway_done_fn done,void *context)
{
    if (s_publish_error != ESP_OK) return s_publish_error;
    assert(s_published_count<64);
    char *copy=malloc(len+1); assert(copy); memcpy(copy,data,len);copy[len]=0;
    s_published[s_published_count++]=copy;
    if(done) done(context,ESP_OK);
    return ESP_OK;
}
esp_err_t gw_business_mqtt_recv_gateway(gw_business_mqtt_owned_message_t *m,uint32_t timeout)
{ (void)m;(void)timeout;return ESP_ERR_TIMEOUT; }
void gw_business_mqtt_message_release(gw_business_mqtt_owned_message_t *m) { (void)m; }
gw_time_point_t gw_time_now(void) { return (gw_time_point_t){1784691000000ULL}; }
esp_err_t gw_gateway_timer_init(void) { return ESP_OK; }
cJSON *gw_gateway_timer_handle(const char *mt,const cJSON *pd)
{ (void)mt;(void)pd;++s_timer_calls;return result_pd(0,"ok"); }
esp_err_t gw_ota_manager_submit_upgrade(const gw_ota_upgrade_request_t *r,gw_result_t *result)
{
    ++s_ota_submits;s_submitted=*r;
    result->code=s_submit_error==ESP_OK?GW_RESULT_OK:GW_RESULT_DEVICE_BUSY;
    result->reason_code=GW_REASON_DEVICE_BUSY;result->detail="busy";
    return s_submit_error;
}
esp_err_t gw_ota_manager_start_pending_upgrade(bool gateway_protocol,const char *request_id)
{
    assert(gateway_protocol && strcmp(request_id,s_submitted.request_id)==0);
    assert(s_published_count>0 || s_publish_error!=ESP_OK);++s_ota_starts;return ESP_OK;
}
void gw_ota_manager_cancel_pending_upgrade(bool gateway_protocol,const char *request_id,const char *reason)
{ assert(gateway_protocol && strcmp(request_id,s_submitted.request_id)==0);(void)reason; }
esp_err_t gw_ota_manager_prepare_rollback(gw_result_t *result)
{ ++s_rollback_calls;result->code=0;return ESP_OK; }
esp_err_t gw_ota_manager_reboot_prepared_rollback(void)
{ assert(s_published_count>0 || s_publish_error!=ESP_OK);++s_reboots;return ESP_OK; }

static void reset(void)
{
    for(size_t i=0;i<s_published_count;++i)free(s_published[i]);
    s_published_count=0;s_timer_calls=0;s_ota_submits=0;s_ota_starts=0;
    s_rollback_calls=0;s_reboots=0;s_submit_error=ESP_OK;s_publish_error=ESP_OK;
    memset(&s_submitted,0,sizeof(s_submitted));
}
static void deliver(const char *payload,bool retained)
{
    gw_business_mqtt_owned_message_t m={.payload=(uint8_t *)payload,.payload_len=strlen(payload),.retained=retained};
    handle_message(&m);
}
static cJSON *last_reply(int code,const char *mt)
{
    assert(s_published_count==1);
    cJSON *r=cJSON_Parse(s_published[0]);assert(r);
    assert(strcmp(cJSON_GetObjectItem(r,"mt")->valuestring,mt)==0);
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(r,"pd"),"code")->valueint==code);
    return r;
}
static void check_reply(int code,const char *mt)
{ cJSON_Delete(last_reply(code,mt)); }

int main(void)
{
    reset();
    deliver("{\"pv\":\"1\",\"dt\":\"gateway\",\"mt\":\"gateway.timer.list\",\"id\":\"\",\"pd\":{}}",false);
    cJSON *r=last_reply(0,"gateway.timer.list.ack");
    assert(strcmp(cJSON_GetObjectItem(r,"id")->valuestring,"")==0);cJSON_Delete(r);assert(s_timer_calls==1);

    reset();
    deliver("{\"pv\":\"1\",\"dt\":\"gateway\",\"mt\":\"gateway.timer.get\",\"id\":{\"bad\":true},\"pd\":{}}",false);
    r=last_reply(1,"gateway.timer.get.ack");assert(cJSON_IsObject(cJSON_GetObjectItem(r,"id")));cJSON_Delete(r);assert(s_timer_calls==0);
    reset();
    deliver("{\"pv\":\"1\",\"dt\":\"gateway\",\"mt\":\"gateway.timer.get\",\"pd\":{}}",false);
    r=last_reply(1,"gateway.timer.get.ack");assert(cJSON_IsNull(cJSON_GetObjectItem(r,"id")));cJSON_Delete(r);
    reset();
    deliver("{\"pv\":1,\"dt\":\"gateway\",\"mt\":\"gateway.timer.get\",\"id\":\"0\",\"pd\":{}}",false);
    check_reply(1,"gateway.timer.get.ack");
    reset();
    deliver("{\"pv\":\"2\",\"dt\":\"gateway\",\"mt\":\"gateway.timer.get\",\"id\":\"0\",\"pd\":{}}",false);
    check_reply(2,"gateway.timer.get.ack");
    reset();
    deliver("{\"pv\":\"1\",\"dt\":\"plc\",\"mt\":\"gateway.timer.get\",\"id\":\"0\",\"pd\":{}}",false);
    check_reply(1,"gateway.timer.get.ack");
    reset();
    deliver("{\"pv\":\"1\",\"dt\":\"gateway\",\"mt\":\"gateway.timer.get\",\"id\":\"12345678901234567890123456789012\",\"pd\":{}}",false);
    check_reply(1,"gateway.timer.get.ack");
    reset();deliver("not JSON",false);deliver("{\"mt\":5}",false);deliver("{} trailing",false);assert(s_published_count==0);
    reset();deliver("{\"mt\":\"plc.read\",\"id\":\"0\"}",false);check_reply(3,"plc.read.ack");assert(s_timer_calls==0);

    const char *rollback="{\"pv\":\"1\",\"dt\":\"plc\",\"mt\":\"gateway.ota.rollback\",\"id\":\" leading 0\",\"pd\":{}}";
    reset();deliver(rollback,true);assert(!s_published_count&&!s_rollback_calls&&!s_reboots);
    reset();deliver(rollback,false);check_reply(0,"gateway.ota.rollback.ack");assert(s_rollback_calls==1&&s_reboots==1);
    reset();deliver("{\"pv\":\"1\",\"dt\":\"plc\",\"mt\":\"gateway.ota.rollback\",\"id\":\"\",\"pd\":{\"x\":1}}",false);
    check_reply(1,"gateway.ota.rollback.ack");assert(!s_rollback_calls&&!s_reboots);
    reset();s_publish_error=ESP_ERR_INVALID_STATE;deliver(rollback,false);assert(!s_published_count&&s_reboots==1);

    const char *ota="{\"pv\":\"1\",\"dt\":\"plc\",\"mt\":\"gateway.ota\",\"id\":\"\",\"pd\":{\"version\":\"same-version\",\"url\":\"https://host/fw.bin\",\"size\":100,\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"progress_report_interval\":0}}";
    reset();deliver(ota,false);check_reply(0,"gateway.ota.ack");assert(s_ota_submits==1&&s_ota_starts==1);
    assert(s_submitted.gateway_protocol&&s_submitted.gateway_progress_interval_sec==0&&s_submitted.request_id[0]==0);
    reset();s_submit_error=ESP_ERR_INVALID_STATE;deliver(ota,false);check_reply(8,"gateway.ota.ack");assert(!s_ota_starts);
    reset();s_publish_error=ESP_ERR_INVALID_STATE;deliver(ota,false);assert(s_ota_starts==1&&!s_published_count);

    reset();cJSON *event=cJSON_CreateObject();assert(gw_gateway_publish_event("gateway.timer.fired","gateway",event,false)==ESP_OK);
    assert(gw_gateway_publish_event("gateway.timer.fired","gateway",event,false)==ESP_OK);cJSON_Delete(event);
    cJSON *a=cJSON_Parse(s_published[0]),*b=cJSON_Parse(s_published[1]);
    const char *aid=cJSON_GetObjectItem(a,"id")->valuestring,*bid=cJSON_GetObjectItem(b,"id")->valuestring;
    assert(strlen(aid)==16&&strlen(bid)==16&&strcmp(aid,bid)!=0);cJSON_Delete(a);cJSON_Delete(b);
    reset();puts("gateway protocol: envelope, routing, retained rollback, OTA acceptance/order and event IDs passed");
    return 0;
}
