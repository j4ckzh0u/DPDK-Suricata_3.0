#define _GNU_SOURCE

#include "util-dpdk-setup.h"
#include "util-dpdk-config.h"
#include "util-dpdk-common.h"
#include "util-error.h"
#include "util-debug.h"
#include "dpdk-include-common.h"
#include "source-dpdkintel.h"

/* D E F I N E S*/
#define SC_DPDK_MAJOR    1
#define SC_DPDK_MINOR    8
#define EAL_ARGS         12


/* E X T E R N */
extern stats_matchPattern_t stats_matchPattern;
extern uint64_t coreSet;

/* G L O B A L S */
uint8_t  portSpeed[16];
uint8_t  portSpeed10;
uint8_t  portSpeed100;
uint8_t  portSpeed1000;
uint8_t  portSpeed10000;
uint8_t  portSpeedUnknown;
uint8_t  dpdkIntelCoreCount = 0;

DpdkIntelPortMap portMap[16];
launchPtr launchFunc[5];

file_config_t file_config;

struct rte_mempool *dp_pktmbuf_pool = NULL;
struct rte_ring    *srb[16];

struct acl4_rule testv4;
struct acl6_rule testv6;

char* argument[EAL_ARGS] = {"suricata","-c","0x1e", "--log-level=1", "--", "-P", "-p", "15", NULL};

/* STATIC */
static const struct rte_eth_conf portConf = {
    .rxmode = {
        .split_hdr_size = 0,
    },
    .txmode = {
        .mq_mode = ETH_MQ_TX_NONE,
    },
};

static struct rte_eth_txconf tx_conf = {
};

static struct rte_eth_rxconf rx_conf = {
    .rx_drop_en = 1,
};

static struct   ether_addr dp_ports_eth_addr [S_DPDK_MAX_ETHPORTS];

void initLaunchFunc(void);

int ringBuffSetup(void)
{
    char srbName [25];
    uint8_t index = 0, maxRing = 16;
    //(DPDKINTEL_GENCFG.Port > SC_RINGBUF)?SC_RINGBUF:DPDKINTEL_GENCFG.Port;

    for (index = 0; index < maxRing; index++)
    {
        sprintf(srbName, "%s%d", "RINGBUFF", index);

        srb [index] = rte_ring_create(srbName, RTE_RING_SIZE, 
                             SOCKET_ID_ANY, RING_F_SP_ENQ);

        if (NULL == srb [index])
        {
            SCLogError(SC_ERR_DPDKINTEL_MEM_FAILED, " Cannot create Ring buff %s", srbName);
            return -1;
        }
        SCLogDebug("Suricata Ring Buffer %s created", srbName);
    }

    return 0;
}

int dpdkPortUnSet(uint8_t portId)
{
    rte_eth_dev_stop(portId);

    SCLogDebug("dev stop done for Port : %u",portId);

    rte_eth_promiscuous_disable(portId);

    return 0;
}

int32_t dpdkIntelDevSetup(void)
{
    uint8_t portIndex = 0, portTotal = rte_eth_dev_count_avail();
    uint8_t inport = 0;
    int32_t ret = 0;
    char portName[RTE_ETH_NAME_MAX_LEN] = {0};

    struct rte_eth_link link;
    struct rte_eth_dev_info dev_info;

    if (unlikely((DPDKINTEL_GENCFG.Port <= 0) || (DPDKINTEL_GENCFG.Port > portTotal))){
        SCLogError(SC_ERR_DPDKINTEL_CONFIG_FAILED, " Ports in DPDK %d Config-file %d", 
                   portTotal, DPDKINTEL_GENCFG.Port);
        return -1;
    }
    SCLogDebug(" - DPDK ports %d config-file ports %d", portTotal, DPDKINTEL_GENCFG.Port);

    dp_pktmbuf_pool =
             rte_mempool_create("mbuf_pool", NB_MBUF,
                        MBUF_SIZE, 32,
                        sizeof(struct rte_pktmbuf_pool_private),
                        rte_pktmbuf_pool_init, NULL,
                        rte_pktmbuf_init, NULL,
                        rte_socket_id()/*SOCKET_ID_ANY*/,
                        0/*MEMPOOL_F_SP_PUT*/);
    if (unlikely(NULL == dp_pktmbuf_pool))
    {
        SCLogError(SC_ERR_DPDKINTEL_MEM_FAILED," mbuf_pool alloc failed");
        return -1;
    }
    SCLogDebug(" - pkt MBUFF setup %p", dp_pktmbuf_pool);

    ret = ringBuffSetup();
    if (ret < 0)
    {
        SCLogError(SC_ERR_DPDKINTEL_MEM_FAILED, " DPDK Ring Buffer setup failed");
        return -11;
    }

    /* check interface PCI information
       ToDo: support for non INTEL PCI interfaces also - phase 2
     */
    for (portIndex = 0; portIndex < DPDKINTEL_GENCFG.Port; portIndex++)
    {
        inport = portMap [portIndex].inport;
        rte_eth_dev_info_get (inport, &dev_info);
        if (rte_eth_dev_get_name_by_port(inport, portName) == 0)
            SCLogDebug(" - port (%u) Name (%s)", inport, portName);
        fflush(stdout);

        /* ToDo - change default configuration to systune configuration */
        ret = rte_eth_dev_configure(inport, 1, 1, &portConf);
        if (ret < 0)
        {
            /* TODO: free mempool */
            SCLogError(SC_ERR_DPDKINTEL_CONFIG_FAILED," configure device: err=%d, port=%u\n",
                  ret, (unsigned) inport);
            return -7;
        }
        SCLogDebug(" - Configured Port %d", inport);

        rte_eth_macaddr_get(inport, 
                           &dp_ports_eth_addr[inport]);

        /* init one RX queue */
        fflush(stdout);
        ret = rte_eth_rx_queue_setup(inport, 0, RTE_TEST_RX_DESC_DEFAULT,
                                     0/*SOCKET_ID_ANY*/,
                                     NULL,
                                     dp_pktmbuf_pool);
        if (ret < 0)
        {
            /* TODO: free mempool */
            SCLogError(SC_ERR_DPDKINTEL_CONFIG_FAILED," rte_eth_rx_queue_setup: err=%d, port=%u\n",
                  ret, (unsigned) inport);
            return -8;
        }
        SCLogDebug(" - RX Queue setup Port %d", inport);

        /* init one TX queue on each port */
        fflush(stdout);
        ret = rte_eth_tx_queue_setup(inport, 0, RTE_TEST_TX_DESC_DEFAULT,
                                     0/*SOCKET_ID_ANY*/,
                                     NULL);
        if (ret < 0)
        {
            SCLogError(SC_ERR_DPDKINTEL_CONFIG_FAILED, " rte_eth_tx_queue_setup:err=%d, port=%u",
                ret, (unsigned) inport);
            return -9;
        }
        SCLogDebug(" - TX Queue setup Port %d", inport);

        /* ToDo: check this from YAML conf file - pahse 2 */
        rte_eth_promiscuous_enable(inport);

        /* check interface link, speed, duplex */
        rte_eth_link_get(inport, &link);
        if (link.link_duplex != ETH_LINK_FULL_DUPLEX) {
            SCLogError(SC_ERR_MISSING_CONFIG_PARAM,
                       " port:%u; duplex:%s, status:%s",
                       (unsigned) inport,
                       (link.link_duplex == ETH_LINK_FULL_DUPLEX)?"Full":"half",
                       (link.link_status == 1)?"up":"down");
            return -10;
        }
        portSpeed[inport] =    (link.link_speed == ETH_SPEED_NUM_10M)?1:
                               (link.link_speed == ETH_SPEED_NUM_100M)?2:
                               (link.link_speed == ETH_SPEED_NUM_1G)?3:
                               (link.link_speed == ETH_SPEED_NUM_10G)?4:
                               (link.link_speed == ETH_SPEED_NUM_20G)?5:
                               (link.link_speed == ETH_SPEED_NUM_40G)?6:
                               0;

        /* ToDo: add support for 20G and 40G */
        if ((link.link_speed == ETH_SPEED_NUM_20G) || 
            (link.link_speed == ETH_SPEED_NUM_40G))
        {
            SCLogError(SC_ERR_DPDKINTEL_CONFIG_FAILED, " Port %u unspported speed %u",
                       inport, portSpeed[inport]);
            return -11;
        }

        {
            (link.link_speed == ETH_SPEED_NUM_10M)?portSpeed10++:
            (link.link_speed == ETH_SPEED_NUM_100M)?portSpeed100++:
            (link.link_speed == ETH_SPEED_NUM_1G)?portSpeed1000++:
            (link.link_speed == ETH_SPEED_NUM_10G)?portSpeed10000++:
            portSpeedUnknown++;
        }

    }

    SCLogDebug("DPDK port setup over!!");
    return 0;
}


void dpdkConfSetup(void)
{
    int32_t ret = 0;
    uint8_t inport = 0, outport = 0, portIndex = 0, portBit = 0;

    SCLogNotice("DPDK Version: %s", rte_version());

    ret = rte_eal_has_hugepages();
    if (unlikely(ret < 0))
    {
        SCLogError(SC_ERR_MISSING_CONFIG_PARAM, "No hugepage configured; %d ", ret);
        rte_panic("ERROR: No Huge Page\n");
        exit(EXIT_FAILURE);
    }

    ret = rte_eal_iopl_init();
    if (ret < 0)
    {
        SCLogError(SC_ERR_MISSING_CONFIG_PARAM, "DPDK IOPL init %d ", ret);
        //rte_panic("ERROR: Cannot init IOPL\n");
        //exit(EXIT_FAILURE);
    }

    /* display default configuration */
    dumpGlobalConfig();

    /* check gloabl configuration meets the requirements */
    if (validateGlobalConfig() != 0) {
        SCLogError(SC_ERR_MISSING_CONFIG_PARAM, "DPDK config validate!!!");
        exit(EXIT_FAILURE);
    }

    /* DPDK Interface setup*/
    if (dpdkIntelDevSetup() != 0) {
        SCLogError(SC_ERR_MISSING_CONFIG_PARAM, "DPDK dev setup!!!");
        exit(EXIT_FAILURE);
    }

    for (portIndex = 0; portIndex < DPDKINTEL_GENCFG.Port; portIndex++) {
        inport  = portMap [portIndex].inport;
        outport = portMap [portIndex].outport;

        if (((portBit >> inport) & 1)  && ((portBit >> outport) & 1 ))
            continue;

        /* check for 10G or smaller */
        if (portSpeed[inport] <= 4) {
           SCLogDebug(" Config core for %d <--> %d", inport, outport);
           dpdkIntelCoreCount++;
        }
        else {
            SCLogError(SC_ERR_DPDKINTEL_CONFIG_FAILED,
                       " Unsupported speed ");
            exit(EXIT_FAILURE);
        }

        if (portSpeed [inport] != portSpeed [outport]) {
            SCLogError(SC_ERR_DPDKINTEL_CONFIG_FAILED,
                      "Mapped ports %d <--> %d Speed Mismatch",
                      inport, outport);
            exit(EXIT_FAILURE);
        }

        portBit |= 1 << inport;
        portBit |= 1 << outport;
    }

    file_config.isDpdk = 1;
    file_config.dpdkCpuCount = rte_eth_dev_count_avail();
    //file_config.dpdkCpuOffset = rte_lcore_count() - DPDKINTEL_GENCFG.Port;
    file_config.dpdkCpuOffset = rte_lcore_count() - dpdkIntelCoreCount;
    file_config.suricataCpuOffset = 0;

    initLaunchFunc();
}

void dpdkAclConfSetup(void)
{
    struct rte_acl_param acl_param;
    struct rte_acl_ctx *ctx;
    
    SCLogNotice("DPDK ACL setup\n");

    acl_param.socket_id = 0;
    acl_param.max_rule_num = 10240 * 2;

    /* setup acl - IPv4 */
    acl_param.rule_size = RTE_ACL_RULE_SZ(RTE_DIM(ip4_defs));
    acl_param.name = "suricata-ipv4";
    ctx = rte_acl_create(&acl_param);
    if (ctx == NULL) {
        SCLogError(SC_ERR_MISSING_CONFIG_PARAM, "acl ipv4 fail!!!");
        exit(EXIT_FAILURE);
    }
    SCLogNotice("DPDK ipv4AclCtx: %p done!", ctx);
    file_config.acl.ipv4AclCtx = (void *)ctx;

    /* setup acl - IPv6 */
    acl_param.rule_size = RTE_ACL_RULE_SZ(RTE_DIM(ip6_defs));
    acl_param.name = "suricata-ipv6";
    ctx = rte_acl_create(&acl_param);
    if (ctx == NULL) {
        SCLogError(SC_ERR_MISSING_CONFIG_PARAM, "acl ipv4 fail!!!");
        exit(EXIT_FAILURE);
    }
    SCLogNotice("DPDK ipv6AclCtx: %p done!", ctx);
    file_config.acl.ipv6AclCtx = (void *)ctx;

}

int32_t addDpdkAcl4Rule(uint32_t srcIp, uint32_t srcIpMask, uint32_t dstIp, uint32_t dstIpMask)
{
    int ret = 0;

    struct rte_acl_rule *rules = (struct rte_acl_rule *) &testv4;
    memset(&testv4, 0, sizeof(testv4));

    testv4.data.category_mask = -1;
    testv4.data.priority = 0xff;
    testv4.data.userdata = 0xdead;

    if (dstIpMask) {
        testv4.field[DST_FIELD_IPV4].value.u32 = dstIp;
        testv4.field[DST_FIELD_IPV4].mask_range.u32 = dstIpMask;
    }
    if (srcIpMask) {
        testv4.field[SRC_FIELD_IPV4].value.u32 = srcIp;
        testv4.field[SRC_FIELD_IPV4].mask_range.u32 = srcIpMask;
    }

    //rte_acl_dump(file_config.acl.ipv4AclCtx);
    ret = rte_acl_add_rules(file_config.acl.ipv4AclCtx, (const struct rte_acl_rule *) rules, 1);
    if (ret != 0) {
       rte_acl_dump(file_config.acl.ipv4AclCtx);
       SCLogNotice("ACL ipv4 add failed %d, but added %u", ret, file_config.acl.ipv4AclCount);
    } else
        file_config.acl.ipv4AclCount += 1;

    return ret;
}

int32_t addDpdkAcl6Rule(uint32_t srcIp[4], uint32_t srcIpMask[4], uint32_t dstIp[4], uint32_t dstIpMask[4])
{
    int ret = 0;

    struct rte_acl_rule *rules = (struct rte_acl_rule *) &testv6;
    memset(&testv6, 0, sizeof(testv6));

    testv6.data.category_mask = -1;
    testv6.data.priority = 0xff;
    testv6.data.userdata = 0xdead;

    if (dstIpMask[0]) {
        testv4.field[IP6_DST0].value.u32 = dstIp[0];
        testv4.field[IP6_DST0].mask_range.u32 = dstIpMask[0];
    }
    if (dstIpMask[1]) {
        testv4.field[IP6_DST1].value.u32 = dstIp[1];
        testv4.field[IP6_DST1].mask_range.u32 = dstIpMask[1];
    }
    if (dstIpMask[2]) {
        testv4.field[IP6_DST2].value.u32 = dstIp[2];
        testv4.field[IP6_DST2].mask_range.u32 = dstIpMask[2];
    }
    if (dstIpMask[3]) {
        testv4.field[IP6_DST3].value.u32 = dstIp[3];
        testv4.field[IP6_DST3].mask_range.u32 = dstIpMask[3];
    }

    if (srcIpMask[0]) {
        testv4.field[IP6_SRC0].value.u32 = srcIp[0];
        testv4.field[IP6_SRC0].mask_range.u32 = srcIpMask[0];
    }

    if (srcIpMask[1]) {
        testv4.field[IP6_SRC1].value.u32 = srcIp[1];
        testv4.field[IP6_SRC1].mask_range.u32 = srcIpMask[1];
    }

    if (srcIpMask[2]) {
        testv4.field[IP6_SRC2].value.u32 = srcIp[2];
        testv4.field[IP6_SRC2].mask_range.u32 = srcIpMask[2];
    }

    if (srcIpMask[3]) {
        testv4.field[IP6_SRC3].value.u32 = srcIp[3];
        testv4.field[IP6_SRC3].mask_range.u32 = srcIpMask[3];
    }

    //rte_acl_dump(file_config.acl.ipv4AclCtx);
    ret = rte_acl_add_rules(file_config.acl.ipv6AclCtx, (const struct rte_acl_rule *) rules, 1);
    if (ret != 0) {
       rte_acl_dump(file_config.acl.ipv6AclCtx);
       SCLogNotice("ACL ipv6 add failed %d, but added %u", ret, file_config.acl.ipv6AclCount);
    } else
        file_config.acl.ipv6AclCount += 1;

    return 0;
}

int32_t addDpdkAcl4Build(void)
{
    int ret = 0;
    struct rte_acl_config acl_build_param = {0};

    acl_build_param.num_categories = 1;
    acl_build_param.num_fields = RTE_DIM(ip4_defs);
    memcpy(&acl_build_param.defs, ip4_defs, sizeof(ip4_defs));

    ret = rte_acl_build(file_config.acl.ipv4AclCtx, &acl_build_param);
    if (ret) {
        rte_acl_dump(file_config.acl.ipv4AclCtx);
        SCLogNotice("ACL ipv4 build failed %d", ret);
    }

    return ret;
}

int32_t addDpdkAcl6Build(void)
{
    int ret = 0;
    struct rte_acl_config acl_build_param = {0};

    acl_build_param.num_categories = 1;
    acl_build_param.num_fields = RTE_DIM(ip6_defs);
    memcpy(&acl_build_param.defs, ip6_defs, sizeof(ip6_defs));

    ret = rte_acl_build(file_config.acl.ipv6AclCtx, &acl_build_param);
    if (ret) {
        rte_acl_dump(file_config.acl.ipv4AclCtx);
        SCLogNotice("ACL ipv6 build failed %d", ret);
    }

    return ret;
}



int32_t dpdkEalInit()
{
    int ret = rte_eal_init(EAL_ARGS, (char **)argument);
    if (ret < 0)
    {
        SCLogError(SC_ERR_MISSING_CONFIG_PARAM, "DPDK EAL init %d ", ret);
        rte_panic("ERROR: Cannot init EAL\n");
        return -1;
    }
    return 0;
}

void dumpMatchPattern(void)
{
    SCLogNotice("----- Match Pattern ----");
    SCLogNotice(" * http:  %"PRId64" ",stats_matchPattern.http);
    SCLogNotice(" * ftp:   %"PRId64" ",stats_matchPattern.ftp);
    SCLogNotice(" * tls:   %"PRId64" ",stats_matchPattern.tls);
    SCLogNotice(" * dns:   %"PRId64" ",stats_matchPattern.dns);
    SCLogNotice(" * smtp:  %"PRId64" ",stats_matchPattern.smtp);
    SCLogNotice(" * ssh:   %"PRId64" ",stats_matchPattern.ssh);
    SCLogNotice(" * smb:   %"PRId64" ",stats_matchPattern.smb);
    SCLogNotice(" * smb2:  %"PRId64" ",stats_matchPattern.smb2);
    SCLogNotice(" * dcerpc:%"PRId64" ",stats_matchPattern.dcerpc);
    SCLogNotice(" * tcp:   %"PRId64" ",stats_matchPattern.tcp);
    SCLogNotice(" * udp:   %"PRId64" ",stats_matchPattern.udp);
    SCLogNotice(" * sctp:  %"PRId64" ",stats_matchPattern.sctp);
    SCLogNotice(" * icmpv6:%"PRId64" ",stats_matchPattern.icmpv6);
    SCLogNotice(" * gre:   %"PRId64" ",stats_matchPattern.gre);
    SCLogNotice(" * raw:   %"PRId64" ",stats_matchPattern.raw);
    SCLogNotice(" * ipv4:  %"PRId64" ",stats_matchPattern.ipv4);
    SCLogNotice(" * ipv6:  %"PRId64" ",stats_matchPattern.ipv6);
    SCLogNotice("-----------------------");

    if (rte_acl_find_existing("suricata-ipv4")) {
        SCLogNotice("----- ACL IPV4 DUMP (%u) ----", file_config.acl.ipv4AclCount);
        rte_acl_dump(file_config.acl.ipv4AclCtx);
        SCLogNotice("-----------------------");
    }

    if (rte_acl_find_existing("suricata-ipv6")) {
        SCLogNotice("----- ACL IPV6 DUMP (%u) ----", file_config.acl.ipv6AclCount);
        rte_acl_dump(file_config.acl.ipv6AclCtx);
        SCLogNotice("-----------------------");
    }

    return;
}

void dumpGlobalConfig(void)
{
    uint8_t index;

    SCLogNotice("----- Global DPDK-INTEL Config -----");
    SCLogNotice(" Number Of Ports  : %d", DPDKINTEL_GENCFG.Port);
    SCLogNotice(" Operation Mode   : %s", ((DPDKINTEL_GENCFG.OpMode == 1) ?"IDS":
                                           (DPDKINTEL_GENCFG.OpMode == 2) ?"IPS":"BYPASS"));
    for (index = 0; index < DPDKINTEL_GENCFG.Port; index++)
    {
        SCLogNotice(" Port:%d, Map:%d", portMap [index].inport, 
                                        portMap [index].outport);
    }
    SCLogNotice("------------------------------------");

    return;
}

uint32_t getCpuCOunt(uint32_t CpuBmp)
{
    uint32_t coreCounts = 0x00;

    do {
        if (CpuBmp)
        {
            coreCounts++;
            CpuBmp = CpuBmp & (CpuBmp - 1);
        }
    } while (CpuBmp);
    
    return coreCounts; 
}

/*  To find the core index from number*/
uint32_t getCpuIndex(void)
{
    static uint32_t cpuIndex = 0;
    unsigned lcore_id = 0;

    struct rte_config *ptr = rte_eal_get_configuration();
    if (ptr == NULL) {
        SCLogError(SC_ERR_DPDKINTEL_CONFIG_FAILED, " Cannot fetch rte_eal_get_configuration");
        return (-1);
    }

    SCLogNotice(" master_lcore %x lcore_count %u", ptr->master_lcore, ptr->lcore_count);
    RTE_LCORE_FOREACH_SLAVE(lcore_id) {
        if ((cpuIndex & (1 << lcore_id)) == 0) {
            cpuIndex |= (1 << lcore_id);
            SCLogNotice(" cpuIndex %x lcore_id %x", cpuIndex, lcore_id);
            return lcore_id;
        }
    }

    SCLogError(SC_ERR_DPDKINTEL_CONFIG_FAILED, " Cannot get right lcore!");
    return (-1);
}

void initLaunchFunc(void)
{
    launchFunc[IDS] = ReceiveDpdkPkts_IDS;
    launchFunc[IPS] = ReceiveDpdkPkts_IPS;
    launchFunc[BYPASS] = ReceiveDpdkPkts_BYPASS;

    return;
}
