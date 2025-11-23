int
sais_list_pcons(struct vhd *vhd)
{
	char json_pcons[LWS_PRE + 8192], *start = json_pcons + LWS_PRE,
	     *p = start, *end = p + sizeof(json_pcons) - LWS_PRE;
	unsigned int ss_flags = LWSSS_FLAG_SOM | LWSSS_FLAG_EOM;
	lws_struct_serialize_t *js;
	struct lwsac *ac = NULL;
	lws_wsmsg_info_t info;
	size_t w;
	lws_struct_json_serialize_result_t r;

	sai_power_managed_builders_t pmb;
	sai_power_controller_t *pc;

	memset(&pmb, 0, sizeof(pmb));

	/* Query PCONs from DB using schema map */
	if (lws_struct_sq3_deserialize(vhd->server.pdb, NULL, "name ",
				       lsm_schema_sq3_map_power_controller,
				       &pmb.power_controllers, &ac, 0, 100)) {
		/* It's okay if empty */
	}

	/* Iterate PCONs and populate controlled builders */
	lws_start_foreach_dll(struct lws_dll2 *, d, pmb.power_controllers.head) {
		pc = lws_container_of(d, sai_power_controller_t, list);
		char filter[128];

		/* Map 'builder_name' column to 'name' field in struct */
		lws_snprintf(filter, sizeof(filter), "where pcon_name = '%s'", pc->name);
		lws_struct_sq3_deserialize(vhd->server.pdb, filter, "builder_name ",
					   lsm_schema_sq3_map_controlled_builder,
					   &pc->controlled_builders_owner, &ac, 0, 100);

	} lws_end_foreach_dll(d);

	/* Serialize */
	js = lws_struct_json_serialize_create(lsm_schema_power_managed_builders,
					      LWS_ARRAY_SIZE(lsm_schema_power_managed_builders),
					      0, &pmb);
	if (!js)
		goto bail;

	do {
		r = lws_struct_json_serialize(js, (uint8_t *)p,
				lws_ptr_diff_size_t(end, p) - 2, &w);
		p += w;

		switch (r) {
		case LSJS_RESULT_FINISH:
			/* fallthru */
		case LSJS_RESULT_CONTINUE:
			memset(&info, 0, sizeof(info));

			info.private_source_idx		= SAI_WEBSRV_PB__GENERATED;
			info.buf			= (uint8_t *)start;
			info.len			= lws_ptr_diff_size_t(p, start);
			info.ss_flags			= ss_flags;

			if (sais_websrv_broadcast_REQUIRES_LWS_PRE(vhd->h_ss_websrv, &info) < 0)
				lwsl_warn("%s: unable to broadcast pcons to web\n", __func__);

			p = start;
			ss_flags &= ~((unsigned int)LWSSS_FLAG_SOM);
			break;

		case LSJS_RESULT_ERROR:
			lws_struct_json_serialize_destroy(&js);
			goto bail;
		}

	} while (r == LSJS_RESULT_CONTINUE);

	lws_struct_json_serialize_destroy(&js);
	lwsac_free(&ac);
	return 0;

bail:
	lwsac_free(&ac);
	return 1;
}

int
sais_list_builders(struct vhd *vhd)
{
	char json_builders[LWS_PRE + 8192], *start = json_builders + LWS_PRE,
	     *p = start, *end = p + sizeof(json_builders) - LWS_PRE,
	     subsequent = 0;
	unsigned int ss_flags = LWSSS_FLAG_SOM;
	lws_dll2_owner_t db_builders_owner;
	lws_struct_serialize_t *js;
	struct lwsac *ac = NULL;
	lws_wsmsg_info_t info;
	sai_plat_t *sp;
	size_t w;

	/* Send PCONs first */
	sais_list_pcons(vhd);

	memset(&db_builders_owner, 0, sizeof(db_builders_owner));

	/* Query builders table, which now includes 'pcon' column */
	if (lws_struct_sq3_deserialize(vhd->server.pdb, NULL, "name ",
				       lsm_schema_sq3_map_plat,
				       &db_builders_owner, &ac, 0, 100)) {
		lwsl_err("%s: Failed to query builders from DB\n", __func__);
		return 1;
	}

	// lwsl_warn("%s: count deserialized %d\n", __func__, (int)db_builders_owner.count);

	p += lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p),
			"{\"schema\":\"com.warmcat.sai.builders\",\"builders\":[");

	lws_start_foreach_dll(struct lws_dll2 *, walk, db_builders_owner.head) {
		lws_struct_json_serialize_result_t r;
		sai_plat_t *live_builder;

		sp = lws_container_of(walk, sai_plat_t, sai_plat_list);
		live_builder = sais_builder_from_uuid(vhd, sp->name);

		if (live_builder) {
			sp->online		= 1;
			lws_strncpy(sp->peer_ip, live_builder->peer_ip,
				    sizeof(sp->peer_ip));
			sp->stay_on	= live_builder->stay_on;
		} else
			sp->online		= 0;

		sp->powering_up			= 0;
		sp->powering_down		= 0;

		lws_start_foreach_dll(struct lws_dll2 *, p, vhd->server.power_state_owner.head) {
			sai_power_state_t *ps = lws_container_of(p, sai_power_state_t, list);
			size_t host_len = strlen(ps->host), pl = strlen(sp->name);

			if ((!strncmp(sp->name, ps->host, host_len) &&
			    sp->name[host_len] == '.') || (pl > host_len &&
					    !strncmp(sp->name + (pl - host_len), ps->host, host_len)))
			{
				lwsl_notice("%s: %s vs %s, sp->online %d, pup %d, pdwn %d\n", __func__, sp->name, ps->host, sp->online, ps->powering_up, ps->powering_down);
				/*
				 * powering_up/down comes to us as a one-shot
				 * notification, we have to clear our copy of it
				 */
				if (sp->online)
					ps->powering_up		= 0;
				else
					ps->powering_down	= 0;

				lwsl_notice("%s: adjusting powering_ %d %d\n", __func__, ps->powering_up, ps->powering_down);
				sp->powering_up		= ps->powering_up;
				sp->powering_down	= ps->powering_down;
				break;
			}
		} lws_end_foreach_dll(p);

		/* Use schema including pcon if available */
		js = lws_struct_json_serialize_create(lsm_schema_map_plat_simple,
						      LWS_ARRAY_SIZE(lsm_schema_map_plat_simple),
						      0, sp);
		if (!js)
			goto bail;

		if (subsequent)
			*p++ = ',';
		subsequent = 1;

		do {
			r = lws_struct_json_serialize(js, (uint8_t *)p,
					lws_ptr_diff_size_t(end, p) - 2, &w);
			p += w;

			switch (r) {
			case LSJS_RESULT_FINISH:
				/* fallthru */
			case LSJS_RESULT_CONTINUE:
				memset(&info, 0, sizeof(info));

				info.private_source_idx		= SAI_WEBSRV_PB__GENERATED;
				info.buf			= (uint8_t *)start;
				info.len			= lws_ptr_diff_size_t(p, start);
				info.ss_flags			= ss_flags;

				if (sais_websrv_broadcast_REQUIRES_LWS_PRE(vhd->h_ss_websrv, &info) < 0)
					lwsl_warn("%s: unable to broadcast to web\n", __func__);

				p = start;
				ss_flags &= ~((unsigned int)LWSSS_FLAG_SOM);
				break;

			case LSJS_RESULT_ERROR:
				lws_struct_json_serialize_destroy(&js);
				goto bail;
			}

		} while (r == LSJS_RESULT_CONTINUE);

		lws_struct_json_serialize_destroy(&js);

	} lws_end_foreach_dll(walk);

	ss_flags |= LWSSS_FLAG_EOM;
	p += lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p), "]}");
	memset(&info, 0, sizeof(info));

	info.private_source_idx		= SAI_WEBSRV_PB__GENERATED;
	info.buf			= (uint8_t *)start;
	info.len			= lws_ptr_diff_size_t(p, start);
	info.ss_flags			= ss_flags;

	if (sais_websrv_broadcast_REQUIRES_LWS_PRE(vhd->h_ss_websrv, &info) < 0)
		lwsl_warn("%s: unable to broadcast to web\n", __func__);

	// lwsl_notice("%s: Broadcasting builder list: %s\n", __func__, start);
	lwsac_free(&ac);
	return 0;

bail:
	lwsac_free(&ac);
	return 1;
}
