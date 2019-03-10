/*
 * Sai builder definitions src/builder/private.h
 *
 * Copyright (C) 2019 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

struct sai_master;
struct sai_builder;

enum sai_lease_states {
	SOS_IDLE,
	SOS_RESERVED,
	SOS_CONFIRMED
};

struct sai_lease {
	struct sai_master *lessor;
	enum sai_lease_states state;
};

struct sai_console {
	struct lws_dll console_list;

	const char *name;
	const char *path;
	const char *mode;
	int baudrate;
};

struct sai_target {
	struct lws_dll target_list;
	struct lws_dll console_head;

	const char *name;

	struct sai_lease lease;

	int timeout;
};

struct sai_master {
	struct lws_dll master_list;

	struct lws *cwsi;

	const char *url;
	const char *interface;
	int priority;

	unsigned int accept_selfsigned:1;
};

struct sai_overlay {
	struct lws_dll overlay_list;

	char *name;
};

struct sai_nspawn {
	struct lws_dll nspawn_list;
	struct lws_dll overlay_head;

	struct sai_builder *builder;

	const char *name;

	struct sai_lease lease;
};

struct sai_builder {
	struct lws_dll master_head;
	struct lws_dll target_head;
	struct lws_dll nspawn_head;

	struct lwsac *conf_head;

	const char *nspawn_path;
	int nspawn_instances;
	int nspawn_timeout;
};

extern const struct lws_protocols protocol_com_warmcat_sai;
extern int sai_builder_config(struct sai_builder *builder, const char *d);
extern void sai_builder_config_destroy(struct sai_builder *builder);
