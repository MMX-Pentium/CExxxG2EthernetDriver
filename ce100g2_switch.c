// SPDX-License-Identifier: GPL-2.0
/*
 * Board glue for the Marvell 88E6190 fitted to the Trend Micro Cloud Edge
 * CE100G2 / Lanner NCA-2011 variants.
 *
 * The switch management bus is exported by the upstream ixgbe driver on the
 * first Intel X553 function.  This module describes the otherwise
 * undiscoverable board wiring to the upstream mv88e6xxx DSA driver.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/errno.h>
#include <linux/if_ether.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/phy.h>
#include <linux/platform_data/mv88e6xxx.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <net/net_namespace.h>

#define CE100G2_X553_DEVICE_ID 0x15c2
#define CE100G2_SWITCH_ADDRESS 0
#define CE100G2_RETRY_DELAY    (2 * HZ)
#define CE100G2_BYPASS_ADDR    0x37
#define CE100G2_DSA_PHY_BUS     "mv88e6xxx-0"

/* 88E6390-family port registers (port addresses 9 and 10). */
#define CE_PORT_STATUS               0x00
#define CE_PORT_STATUS_CMODE_MASK    0x000f
#define CE_PORT_STATUS_CMODE_2500BX  0x000b
#define CE_PORT_MAC_CTL              0x01
#define CE_PORT_MAC_FORCE_SPEED      0x2000
#define CE_PORT_MAC_ALT_SPEED        0x1000
#define CE_PORT_MAC_LINK_UP          0x0020
#define CE_PORT_MAC_FORCE_LINK       0x0010
#define CE_PORT_MAC_DUPLEX_FULL      0x0008
#define CE_PORT_MAC_FORCE_DUPLEX     0x0004
#define CE_PORT_MAC_SPEED_MASK       0x0003
#define CE_PORT_MAC_SPEED_2500       0x0003
#define CE_PORT_MAC_MANAGED_MASK     0x303f

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "load even when the Lanner board DMI name is absent");

static ushort switch_address = CE100G2_SWITCH_ADDRESS;
module_param(switch_address, ushort, 0444);
MODULE_PARM_DESC(switch_address, "88E6190 MDIO address (default: 0)");

static uint retries = 30;
module_param(retries, uint, 0444);
MODULE_PARM_DESC(retries, "number of two-second attachment attempts");

static bool force_cpu_links = true;
module_param(force_cpu_links, bool, 0444);
MODULE_PARM_DESC(force_cpu_links,
		 "force switch CPU ports 9/10 to 2500base-x full duplex");

static bool restart_cpu_phys = true;
module_param(restart_cpu_phys, bool, 0444);
MODULE_PARM_DESC(restart_cpu_phys,
		 "restart switch CPU-port PHYs when their PCS is not ready");

static int bypass_i2c_bus = -1;
module_param(bypass_i2c_bus, int, 0444);
MODULE_PARM_DESC(bypass_i2c_bus,
		 "I801 I2C bus for the LAN8/WAN bypass relay (-1: auto-detect)");

static struct mdio_device *ce_switch;
static struct net_device *ce_master[2];
static struct delayed_work ce_attach_work;
static uint ce_attempt;
static bool ce_bypass_ready;

static char *ce_port_names[] = {
	[1] = "lan1",
	[2] = "lan2",
	[3] = "lan3",
	[4] = "lan4",
	[5] = "lan5",
	[6] = "lan6",
	[7] = "lan7",
	[8] = "lan8",
	[9] = "cpu",
	[10] = "cpu",
};

static struct dsa_mv88e6xxx_pdata ce_pdata = {
	.compatible = "marvell,mv88e6190",
	.enabled_ports = GENMASK(10, 1),
	.irq = -1,
};

static const struct dmi_system_id ce100g2_dmi_table[] = {
	{ .ident = "Lanner NCA-2011Z-TM2J",
	  .matches = { DMI_MATCH(DMI_PRODUCT_NAME, "NCA-2011Z-TM2J") } },
	{ .ident = "Lanner NCA-2011Z-TM2E",
	  .matches = { DMI_MATCH(DMI_PRODUCT_NAME, "NCA-2011Z-TM2E") } },
	{ .ident = "Lanner NCA-2011Z-TM3J",
	  .matches = { DMI_MATCH(DMI_PRODUCT_NAME, "NCA-2011Z-TM3J") } },
	{ .ident = "Lanner NCA-2011Z-TM3E",
	  .matches = { DMI_MATCH(DMI_PRODUCT_NAME, "NCA-2011Z-TM3E") } },
	{ }
};
MODULE_DEVICE_TABLE(dmi, ce100g2_dmi_table);

static int ce_bypass_write(struct i2c_adapter *adap, u8 command, u8 value)
{
	union i2c_smbus_data data = { .byte = value };

	return i2c_smbus_xfer(adap, CE100G2_BYPASS_ADDR, 0,
			      I2C_SMBUS_WRITE, command,
			      I2C_SMBUS_BYTE_DATA, &data);
}

static struct i2c_adapter *ce_find_bypass_adapter(void)
{
	struct i2c_adapter *adap;
	int nr;

	if (bypass_i2c_bus >= 0)
		return i2c_get_adapter(bypass_i2c_bus);

	/* The relay MCU is connected to the Atom C3000 I801 controller.  Bus
	 * numbers are normally 1, but discover it by adapter name so another
	 * firmware-enumerated I2C controller cannot change the result.
	 */
	for (nr = 0; nr < 32; nr++) {
		adap = i2c_get_adapter(nr);
		if (!adap)
			continue;
		if (strstr(adap->name, "I801"))
			return adap;
		i2c_put_adapter(adap);
	}

	return NULL;
}

static int ce_configure_bypass(void)
{
	struct i2c_adapter *adap;
	int ret;

	adap = ce_find_bypass_adapter();
	if (!adap)
		return -EPROBE_DEFER;

	/* This is the product OS's "bypass off" state: retain fail-open bypass
	 * for a power loss (0x10 = 3), but connect LAN8 and WAN to their two
	 * NICs while the appliance is powered (0x51 = 0).
	 */
	ret = ce_bypass_write(adap, 0x51, 0x00);
	if (!ret)
		ret = ce_bypass_write(adap, 0x10, 0x03);
	if (!ret)
		pr_info("LAN8/WAN bypass relay set to powered-on NIC mode on i2c-%d\n",
			adap->nr);

	i2c_put_adapter(adap);
	return ret;
}

static int ce_mdio_bus_match(struct device *dev, struct device_driver *drv)
{
	struct mdio_device *mdiodev = to_mdio_device(dev);

	return !strcmp(mdiodev->modalias, drv->name);
}

static struct net_device *ce_find_netdev(struct pci_dev *pdev)
{
	struct net_device *ndev;
	struct net_device *found = NULL;

	rtnl_lock();
	for_each_netdev(&init_net, ndev) {
		if (ndev->dev.parent == &pdev->dev) {
			dev_hold(ndev);
			found = ndev;
			break;
		}
	}
	rtnl_unlock();

	return found;
}

static void ce_put_masters(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ce_master); i++) {
		if (ce_master[i]) {
			dev_put(ce_master[i]);
			ce_master[i] = NULL;
		}
	}
}

static int ce_configure_cpu_port(struct mii_bus *bus, int port)
{
	int old;
	u16 val;
	int ret;

	/* Legacy DSA platform data has no phy-mode field.  Consequently the
	 * upstream mv88e6xxx driver cannot call mv88e6390_port_set_cmode() for
	 * these CPU ports.  Select the board's actual 2500BASE-X wiring here.
	 */
	old = mdiobus_read(bus, port, CE_PORT_STATUS);
	if (old < 0)
		return old;

	val = (old & ~CE_PORT_STATUS_CMODE_MASK) |
	      CE_PORT_STATUS_CMODE_2500BX;
	if (val != old) {
		ret = mdiobus_write(bus, port, CE_PORT_STATUS, val);
		if (ret)
			return ret;
	}
	pr_info("CPU port %d status/cmode %#06x -> %#06x\n", port, old, val);

	old = mdiobus_read(bus, port, CE_PORT_MAC_CTL);
	if (old < 0)
		return old;

	/* Equivalent to mv88e6390_port_set_speed_duplex(2500, FULL)
	 * followed by mv88e6xxx_port_set_link(LINK_FORCED_UP).
	 */
	val = old & ~CE_PORT_MAC_MANAGED_MASK;
	val |= CE_PORT_MAC_FORCE_SPEED | CE_PORT_MAC_ALT_SPEED |
	       CE_PORT_MAC_LINK_UP | CE_PORT_MAC_FORCE_LINK |
	       CE_PORT_MAC_DUPLEX_FULL | CE_PORT_MAC_FORCE_DUPLEX |
	       CE_PORT_MAC_SPEED_2500;

	ret = mdiobus_write(bus, port, CE_PORT_MAC_CTL, val);
	if (!ret)
		pr_info("CPU port %d MAC control %#06x -> %#06x\n",
			port, old, val);

	return ret;
}

static int ce_restart_cpu_phys_if_needed(void)
{
	struct mii_bus *bus;
	int control;
	int status;
	int port;
	int ret = 0;

	bus = mdio_find_bus(CE100G2_DSA_PHY_BUS);
	if (!bus)
		return -EPROBE_DEFER;

	/* The product mv88e6190 driver checks MMD 4 register 0xa003 when a
	 * CPU master is brought up.  Some NCA-2011Z-TM2J units remain at
	 * 0xa800 after DSA setup; writing 0x8140 to MMD 4 register 0x2000
	 * restarts that PCS and changes both X553 links to 2.5G immediately.
	 */
	for (port = 9; port <= 10; port++) {
		control = mdiobus_c45_read(bus, port, 4, 0x2000);
		if (control < 0) {
			ret = control;
			break;
		}

		status = mdiobus_c45_read(bus, port, 4, 0xa003);
		if (status < 0) {
			ret = status;
			break;
		}

		pr_info("CPU port %d PHY MMD4 control %#06x status %#06x\n",
			port, control, status);
		if (status == 0xac20)
			continue;

		ret = mdiobus_c45_write(bus, port, 4, 0x2000, 0x8140);
		if (ret)
			break;
		pr_info("CPU port %d PHY restarted\n", port);
	}

	put_device(&bus->dev);
	return ret;
}

static int ce_attach(void)
{
	struct pci_dev *pdev0 = NULL;
	struct pci_dev *pdev1 = NULL;
	struct mdio_device *mdiodev;
	struct mii_bus *bus;
	char bus_id[MII_BUS_ID_SIZE];
	int ret;
	int port;

	pdev0 = pci_get_device(PCI_VENDOR_ID_INTEL,
			       CE100G2_X553_DEVICE_ID, NULL);
	while (pdev0 && PCI_FUNC(pdev0->devfn) != 0)
		pdev0 = pci_get_device(PCI_VENDOR_ID_INTEL,
				       CE100G2_X553_DEVICE_ID, pdev0);
	if (!pdev0)
		return -ENODEV;

	pdev1 = pci_get_domain_bus_and_slot(pci_domain_nr(pdev0->bus),
					    pdev0->bus->number,
					    PCI_DEVFN(PCI_SLOT(pdev0->devfn), 1));
	if (!pdev1 || pdev1->vendor != PCI_VENDOR_ID_INTEL ||
	    pdev1->device != CE100G2_X553_DEVICE_ID) {
		ret = -ENODEV;
		goto out_put_pci;
	}

	ce_master[0] = ce_find_netdev(pdev0);
	ce_master[1] = ce_find_netdev(pdev1);
	if (!ce_master[0] || !ce_master[1]) {
		ret = -EPROBE_DEFER;
		goto out_put_masters;
	}

	snprintf(bus_id, sizeof(bus_id), "ixgbe-mdio-%s", pci_name(pdev0));
	bus = mdio_find_bus(bus_id);
	if (!bus) {
		ret = -EPROBE_DEFER;
		goto out_put_masters;
	}

	memset(&ce_pdata.cd, 0, sizeof(ce_pdata.cd));
	for (port = 1; port <= 10; port++)
		ce_pdata.cd.port_names[port] = ce_port_names[port];
	ce_pdata.cd.netdev[9] = &ce_master[0]->dev;
	ce_pdata.cd.netdev[10] = &ce_master[1]->dev;
	ce_pdata.netdev = ce_master[0];

	mdiodev = mdio_device_create(bus, switch_address);
	if (IS_ERR(mdiodev)) {
		ret = PTR_ERR(mdiodev);
		put_device(&bus->dev);
		goto out_put_masters;
	}

	strscpy(mdiodev->modalias, "mv88e6085", sizeof(mdiodev->modalias));
	mdiodev->bus_match = ce_mdio_bus_match;
	mdiodev->dev.platform_data = &ce_pdata;

	request_module("mv88e6xxx");
	ret = mdio_device_register(mdiodev);
	if (ret) {
		put_device(&bus->dev);
		mdio_device_free(mdiodev);
		goto out_put_masters;
	}
	if (!mdiodev->dev.driver) {
		/* mv88e6xxx_probe() consumes pdata->netdev's reference on error. */
		ce_master[0] = NULL;
		pr_err("mv88e6xxx did not bind to %s:%u; probe failed\n",
		       bus_id, switch_address);
		mdio_device_remove(mdiodev);
		put_device(&bus->dev);
		ret = -ENODEV;
		goto out_put_masters;
	}

	if (force_cpu_links) {
		ret = ce_configure_cpu_port(bus, 9);
		if (!ret)
			ret = ce_configure_cpu_port(bus, 10);
		if (ret)
			pr_err("failed to configure switch CPU links: %d\n", ret);
	}
	put_device(&bus->dev);

	if (restart_cpu_phys) {
		ret = ce_restart_cpu_phys_if_needed();
		if (ret)
			pr_err("failed to restart switch CPU PHYs: %d\n", ret);
	}

	ce_switch = mdiodev;
	pr_info("attached 88E6190 at %s:%u; masters %s and %s\n",
		bus_id, switch_address, ce_master[0]->name, ce_master[1]->name);
	ret = 0;
	goto out_put_pci;

out_put_masters:
	ce_put_masters();
out_put_pci:
	pci_dev_put(pdev1);
	pci_dev_put(pdev0);
	return ret;
}

static void ce_attach_worker(struct work_struct *work)
{
	int bypass_ret = 0;
	int switch_ret = 0;

	if (!ce_bypass_ready) {
		bypass_ret = ce_configure_bypass();
		if (!bypass_ret)
			ce_bypass_ready = true;
	}

	if (!ce_switch)
		switch_ret = ce_attach();

	if (ce_bypass_ready && ce_switch)
		return;

	ce_attempt++;
	if (ce_attempt < retries) {
		if (ce_attempt == 1)
			pr_info("waiting for bypass SMBus and both X553 netdevs/MDIO\n");
		schedule_delayed_work(&ce_attach_work, CE100G2_RETRY_DELAY);
	} else {
		pr_err("setup incomplete after %u attempts (bypass %d, switch %d)\n",
		       ce_attempt, bypass_ret, switch_ret);
	}
}

static int __init ce100g2_init(void)
{
	if (!force && !dmi_check_system(ce100g2_dmi_table)) {
		pr_err("unsupported DMI product; use force=1 only on known hardware\n");
		return -ENODEV;
	}

	if (switch_address > 31)
		return -EINVAL;

	request_module("i2c_i801");
	INIT_DELAYED_WORK(&ce_attach_work, ce_attach_worker);
	schedule_delayed_work(&ce_attach_work, 0);
	return 0;
}

static void __exit ce100g2_exit(void)
{
	cancel_delayed_work_sync(&ce_attach_work);
	if (ce_switch) {
		mdio_device_remove(ce_switch);
		ce_switch = NULL;
	}
	ce_put_masters();
}

module_init(ce100g2_init);
module_exit(ce100g2_exit);

MODULE_AUTHOR("CE100G2Driver project");
MODULE_DESCRIPTION("Trend Micro CE100G2 / Lanner NCA-2011 88E6190 board glue");
MODULE_VERSION("0.1.10");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: ixgbe mv88e6xxx");
