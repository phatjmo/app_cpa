/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2015, LeaseHawk, LLC.
 *
 * Justin Zimmer (jzimmer@leasehawk.com)
 *
 * Structure stolen from app_amd.c by Claude Klimos (claude.klimos@aheeva.com) since I suck at C.
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 *
 * A license has been granted to Digium (via disclaimer) for the use of
 * this code.
 */

/*! \file
 *
 * \Call Progress Analysis for Connected PBX Calls
 *
 * \author Justin Zimmer (jzimmer@leasehawk.com)
 * \author Claude Klimos (claude.klimos@aheeva.com)
 *
 * \ingroup applications
 */

/*! \li \ref app_cpa.c uses the configuration file \ref cpa.conf
 * \addtogroup configuration_file Configuration Files
 */

/*! 
 * \page cpa.conf cpa.conf
 * \verbinclude cpa.conf.sample
 */

/*** MODULEINFO
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/module.h"
#include "asterisk/lock.h"
#include "asterisk/channel.h"
#include "asterisk/frame.h"
#include "asterisk/dsp.h"
#include "asterisk/pbx.h"
#include "asterisk/config.h"
#include "asterisk/app.h"
#include "asterisk/format_cache.h"

/*** DOCUMENTATION
	<application name="CPA" language="en_US">
		<synopsis>
			Attempt DSP call progress analysis on a connected channel (What chan_dahdi does, but for channel technologies that rely on signalling for CPA.
			This is designed for cases where the called party picks up immediately, like a PBX, and then proceeds into its own internal ring group or voicemail.
		</synopsis>
		<syntax>
			<parameter name="silenceThreshold" required="false">
				<para>How long do we consider silence</para>
				<para>Default is 250ms</para>
			</parameter>
			<parameter name="totalAnalysis Time" required="false">
				<para>Is the maximum time allowed for the algorithm</para>
				<para>Default is 5000ms</para>
			</parameter>
		</syntax>
		<description>
			<para>
				Attempt DSP call progress analysis on a connected channel (What chan_dahdi does, but for channel technologies that rely on signalling for CPA.
				This is designed for cases where the called party picks up immediately, like a PBX, and then proceeds into its own internal ring group or voicemail.
				This concept allows an application to detect if the connect was sent to ringing or if the call was picked up immediately so that a ring timeout can be maintained,
				and other actions attempted in the dialplan. 
				Consider the following: Alice calls Bob and RoboBob answers the channel and attempts a ring group to find Bob in the office and plays ringback to Alice although the channel is technically connected.
				If Alice is the name of an AGI based call handling system, then she might want to try another destination for Bob, or switch to an automated system.
				If Alice cannot tell the difference between a PBX ringing or a live person answering because the PBX answered on a SIP trunk with 200OK then she will think Bob actually answered.
				If Alice could run a DSP based call progress on the channel, like an FXO channel normally would, she would be able to make a decision whether to attempt Bob's cell phone after a certain number of rings.
				This app uses the ast_dsp_call_progress function in dsp.c to get an AST_FRAME_CONTROL type response and return this result to the dialplan or AGI application.
				Also, if the channel connects but plays a busy tone over the channel, the application will never know this on technologies that rely on signalling for call progress.
			</para>
			<para>This application sets the following channel variables:</para>
			<variablelist>
				<variable name="CPASTATUS">
					<para>This is the status of the call progress analysis</para>
					<value name="Ringing" />
					<value name="Busy" />
					<value name="Hungup" />
					<value name="Congestion" />
					<value name="Talking" />
				</variable>
			</variablelist>
		</description>
		<see-also>
			<ref type="application">CPA</ref>
			<ref type="application">AMD</ref>
			<ref type="application">WaitForSilence</ref>
			<ref type="application">WaitForNoise</ref>
		</see-also>
	</application>

 ***/

static const char app[] = "CPA";

/* Set to the lowest ms value provided in cpa.conf or application parameters */

static int dfltMaxWaitTimeForFrame  = 50;
static int dfltSilenceThreshold     = 100;
static int dfltTotalAnalysisTime    = 1000;

void cpa2str(char cpaString[256], int cpa);
void tone2str(char toneString[256], int tone);

static void callProgress(struct ast_channel *chan, const char *data)
{
	int res = 0;
	struct ast_frame *f = NULL;
	//struct ast_frame *cpaf = NULL;
	struct ast_dsp *cpadsp = NULL;
	int dspnoise = 0, framelength = 0;
	int iTotalTime = 0;
	//int noiseDuration = 0;
	int toneState = 0;	
	int lastTone = 0;
	int tcount = 0;
	RAII_VAR(struct ast_format *, readFormat, NULL, ao2_cleanup);
	char cpaStatus[256] = "";
	//char toneStatus[256] = "";
	char *parse = ast_strdupa(data);
	//char progzone[10] = "us";
	
	/* Set defaults for tone chunks */
	int THRESH_SILENCE = 12;	/*!< Silence for at least 240ms */
	int THRESH_RING = 8;		/*!< Need at least 150ms ring to accept */
	int THRESH_TALK = 2;		/*!< Talk detection does not work continuously */
	int THRESH_BUSY = 4;		/*!< Need at least 80ms to accept */
	int THRESH_CONGESTION = 4;		/*!< Need at least 80ms to accept */
	int THRESH_HANGUP = 60;		/*!< Need at least 1300ms to accept hangup */

	/* Lets set the initial values of the variables that will control the algorithm.
	   The initial values are the default ones. If they are passed as arguments
	   when invoking the application, then the default values will be overwritten
	   by the ones passed as parameters. */
	int maxWaitTimeForFrame  = dfltMaxWaitTimeForFrame;
	int silenceThreshold     = dfltSilenceThreshold;
	int totalAnalysisTime    = dfltTotalAnalysisTime;	

	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(argSilenceThreshold);
		AST_APP_ARG(argTotalAnalysisTime);
	);

	if (!ast_strlen_zero(parse)) {
		/* Some arguments have been passed. Lets parse them and overwrite the defaults. */
		AST_STANDARD_APP_ARGS(args, parse);
		if (!ast_strlen_zero(args.argSilenceThreshold))
			silenceThreshold = atoi(args.argSilenceThreshold);
		if (!ast_strlen_zero(args.argTotalAnalysisTime))
			totalAnalysisTime = atoi(args.argTotalAnalysisTime);	
	} else {
		ast_debug(1, "CPA using the default parameters.\n");
	}

	if (maxWaitTimeForFrame > totalAnalysisTime)
		maxWaitTimeForFrame = totalAnalysisTime;

	/* Now we're ready to roll! */
	ast_verb(3, "CPA: maxWaitTimeForFrame [%d] silenceThreshold [%d] totalAnalysisTime [%d]\n",
				maxWaitTimeForFrame, silenceThreshold, totalAnalysisTime);
	

	/*! All THRESH_XXX values are in GSAMP_SIZE chunks (us = 22ms) */
	THRESH_SILENCE = silenceThreshold / 20;	/*!< Silence for at least 240ms */
	//THRESH_RING = 8;		/*!< Need at least 150ms ring to accept */
	//THRESH_TALK = 2;		/*!< Talk detection does not work continuously */
	//THRESH_BUSY = 4;		/*!< Need at least 80ms to accept */
	//THRESH_CONGESTION = 4;		/*!< Need at least 80ms to accept */
	//THRESH_HANGUP = 60;		/*!< Need at least 1300ms to accept hangup */
	//THRESH_RING2ANSWER = 300;	/*!< Timeout from start of ring to answer (about 6600 ms) */
	
	

	/* Set read format to signed linear so we get signed linear frames in */
	readFormat = ao2_bump(ast_channel_readformat(chan));
	if (ast_set_read_format(chan, ast_format_slin) < 0 ) {
		ast_log(LOG_WARNING, "CPA: Channel [%s]. Unable to set to linear mode, giving up\n", ast_channel_name(chan));
		pbx_builtin_setvar_helper(chan , "CPASTATUS", "");
		return;
	}

	/* Create a new DSP for call progress */
	if (!(cpadsp = ast_dsp_new())) {
		ast_log(LOG_WARNING, "CPA: Channel [%s]. Unable to create DSP :(\n", ast_channel_name(chan));
		pbx_builtin_setvar_helper(chan , "CPASTATUS", "");
		return;
	}
	//ast_dsp_set_call_progress_zone(cpadsp, progzone);

	/* Set silence threshold to specified value */
	//ast_debug(1, "CPA setting silence threshold: [%d]\n", silenceThreshold);
	//ast_dsp_set_threshold(cpadsp, silenceThreshold);

	/* Now we go into a loop waiting for frames from the channel */
	while ((res = ast_waitfor(chan, 2 * maxWaitTimeForFrame)) > -1) {

		/* If we fail to read in a frame, that means they hung up */
		if (!(f = ast_read(chan))) {
			ast_verb(3, "CPA: Channel [%s]. Hungup\n", ast_channel_name(chan));
			ast_debug(1, "Got hangup\n");
			strcpy(cpaStatus, "Hungup");
			res = 1;
			break;
		}

		ast_debug(1, "CPA checking frametype: [%d].\n", f->frametype);

		if (f->frametype == AST_FRAME_VOICE || f->frametype == AST_FRAME_NULL || f->frametype == AST_FRAME_CNG) {
			/* If the total time exceeds the analysis time then give up as we are not too sure */
			if (f->frametype == AST_FRAME_VOICE) {
				framelength = (ast_codec_samples_count(f) / DEFAULT_SAMPLES_PER_MS);
				ast_debug(1, "Frametype = AST_FRAME_VOICE. Framelength = [%d]\n", framelength);
			} else {
				framelength = 2 * maxWaitTimeForFrame;
				ast_debug(1, "Frametype != AST_FRAME_VOICE. Framelength = [%d]\n", framelength);
			}

			iTotalTime += framelength;
			if (iTotalTime >= totalAnalysisTime) {
				ast_verb(3, "CPA: Channel [%s]. Too long...\n", ast_channel_name(chan));
				ast_frfree(f);
				strcpy(cpaStatus , "Unknown");
				break;
			}

			ast_debug(1, "CPA Checking Call Progress.\n");
			if (ast_dsp_call_progress(cpadsp, f) > 0){
				ast_debug(1, "CPA: Wait what? Frame Control came back as NOT SILENCE on channel [%s]\n", ast_channel_name(chan));
			}

			ast_debug(1, "CPA pulling tonestate.\n");
			toneState = ast_dsp_get_tstate(cpadsp);
			ast_debug(1, "CPA Frame - Frametype: [%d] Subclass: [%d] DSP ToneState: [%d]\n", f->frametype, f->subclass.integer, toneState);
			
			if (toneState == lastTone){
				tcount = ast_dsp_get_tcount(cpadsp);
				ast_debug(1, "CPA ToneState Repeated - lastTone: [%d] toneState: [%d] tcount: [%d]\n", lastTone, toneState, tcount);
				switch (toneState) {
					case DSP_TONE_STATE_RINGING:
						if (tcount == THRESH_RING) {
							strcpy(cpaStatus, "Ringing");
							ast_debug(1, "CPA Result - Channel: [%s] CPAStatus: [%s]\n", ast_channel_name(chan), cpaStatus);
							res = 1;
						}
						break;
					case DSP_TONE_STATE_SILENCE:
						if (tcount > THRESH_SILENCE) {
							strcpy(cpaStatus, "Silence");
							ast_debug(1, "CPA Result - Channel: [%s] CPAStatus: [%s]\n", ast_channel_name(chan), cpaStatus);
							//res = 1;
						}
						break;
					case DSP_TONE_STATE_BUSY:
						if (tcount == THRESH_BUSY) {
							strcpy(cpaStatus, "Busy");
							ast_debug(1, "CPA Result - Channel: [%s] CPAStatus: [%s]\n", ast_channel_name(chan), cpaStatus);
							res = 1;
						}
						break;
					case DSP_TONE_STATE_TALKING:
						if (tcount == THRESH_TALK) {
							strcpy(cpaStatus, "Talking");
							ast_debug(1, "CPA Result - Channel: [%s] CPAStatus: [%s]\n", ast_channel_name(chan), cpaStatus);
							res = 1;
						}
						break;
					case DSP_TONE_STATE_SPECIAL3:
						if (tcount == THRESH_CONGESTION) {
							strcpy(cpaStatus, "Congestion");
							ast_debug(1, "CPA Result - Channel: [%s] CPAStatus: [%s]\n", ast_channel_name(chan), cpaStatus);
							res = 1;
						}
						break;
					case DSP_TONE_STATE_HUNGUP:
						if (tcount == THRESH_HANGUP) {
							strcpy(cpaStatus, "Hungup");
							ast_debug(1, "CPA Result - Channel: [%s] CPAStatus: [%s]\n", ast_channel_name(chan), cpaStatus);
							res = 1;
						}
						break;
				}
				if(res == 1) {
					break;
				}
			} else {
				ast_debug(1, "Stop state %d with duration %d\n", lastTone, tcount);
				ast_debug(1, "Start state %d\n", toneState);
				lastTone = toneState;
				tcount = 1;
			}


		}
		//ast_debug(1, "dspnoise: [%dms]\n", dspnoise);
		ast_frfree(f);

	}

	ast_debug(1, "Frame Read For: [%dms], CPA returned: [%s]\n", dspnoise, cpaStatus);
	
	if (!res) {
		/* It took too long to get a frame back. Giving up. */
		ast_verb(3, "CPA: Channel [%s]. Too long...\n", ast_channel_name(chan));
		strcpy(cpaStatus , "NOTSURE");
	}

	/* Set the status and cause on the channel */
	pbx_builtin_setvar_helper(chan , "CPASTATUS" , cpaStatus);
	ast_verb(3, "CPA: Channel [%s] - Frame Length: [%d] - iTotalTime: [%d]\n", ast_channel_name(chan), framelength, iTotalTime);

	/* Restore channel read format */
	if (readFormat && ast_set_read_format(chan, readFormat))
		ast_log(LOG_WARNING, "CPA: Unable to restore read format on '%s'\n", ast_channel_name(chan));

	/* Free the DSP used to detect silence */
	ast_dsp_free(cpadsp);

	return;
}			

void cpa2str(char cpaString[256], int cpa)
{
	//char cpaString[256] = "";
	switch (cpa) {
		case 0:
			strcpy(cpaString, "Silence");
			break;
		case AST_CONTROL_HANGUP:
			strcpy(cpaString, "Hangup");
			break;			
		case AST_CONTROL_RINGING:
			strcpy(cpaString, "Ringing");
			break;
		case AST_CONTROL_ANSWER:
			strcpy(cpaString, "Answer");
			break;
		case AST_CONTROL_BUSY:
			strcpy(cpaString, "Busy");
			break;
		case AST_CONTROL_CONGESTION:
			strcpy(cpaString, "Congestion");
			break;
		case -1:
			strcpy(cpaString, "Negative One! Uh Oh!!!");
			break;
		default:
			ast_log(LOG_WARNING, "Unknown CPA: '%d'", cpa);
			strcpy(cpaString, "Unknown");

		}
}

void tone2str(char toneString[256], int tone)
{
	//char cpaString[256] = "";
	switch (tone) {
		case DSP_TONE_STATE_SILENCE:
			strcpy(toneString, "Silence");
			break;
		case DSP_TONE_STATE_HUNGUP:
			strcpy(toneString, "Hungup");
			break;			
		case DSP_TONE_STATE_RINGING:
			strcpy(toneString, "Ringing");
			break;
		case DSP_TONE_STATE_TALKING:
			strcpy(toneString, "Talking");
			break;
		case DSP_TONE_STATE_BUSY:
			strcpy(toneString, "Busy");
			break;
		case DSP_TONE_STATE_SPECIAL1:
			strcpy(toneString, "Special1");
			break;
		case DSP_TONE_STATE_SPECIAL2:
			strcpy(toneString, "Special2");
			break;
		case DSP_TONE_STATE_SPECIAL3:
			strcpy(toneString, "Special3");
			break;						
		case -1:
			strcpy(toneString, "Negative One! Uh Oh!!!");
			break;
		default:
			ast_log(LOG_WARNING, "Unknown Tone: '%d'", tone);
			strcpy(toneString, "Unknown");

		}
}

static int cpa_exec(struct ast_channel *chan, const char *data)
{
	callProgress(chan, data);

	return 0;
}

static int load_config(int reload)
{
	struct ast_config *cfg = NULL;
	char *cat = NULL;
	struct ast_variable *var = NULL;
	struct ast_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	dfltSilenceThreshold = ast_dsp_get_threshold_from_settings(THRESHOLD_SILENCE);

	if (!(cfg = ast_config_load("cpa.conf", config_flags))) {
		ast_log(LOG_ERROR, "Configuration file cpa.conf missing.\n");
		return -1;
	} else if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		return 0;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_ERROR, "Config file cpa.conf is in an invalid format.  Aborting.\n");
		return -1;
	}

	cat = ast_category_browse(cfg, NULL);

	while (cat) {
		if (!strcasecmp(cat, "general") ) {
			var = ast_variable_browse(cfg, cat);
			while (var) {
				if (!strcasecmp(var->name, "silence_threshold")) {
					dfltSilenceThreshold = atoi(var->value);
				} else if (!strcasecmp(var->name, "total_analysis_time")) {
					dfltTotalAnalysisTime = atoi(var->value);
				} else {
					ast_log(LOG_WARNING, "%s: Cat:%s. Unknown keyword %s at line %d of cpa.conf\n",
						app, cat, var->name, var->lineno);
				}
				var = var->next;
			}
		}
		cat = ast_category_browse(cfg, cat);
	}

	ast_config_destroy(cfg);

	ast_verb(3, "CPA defaults: totalAnalysisTime [%d] silenceThreshold [%d]\n",	dfltTotalAnalysisTime, dfltSilenceThreshold);

	return 0;
}

static int unload_module(void)
{
	return ast_unregister_application(app);
}

/*!
 * \brief Load the module
 *
 * Module loading including tests for configuration or dependencies.
 * This function can return AST_MODULE_LOAD_FAILURE, AST_MODULE_LOAD_DECLINE,
 * or AST_MODULE_LOAD_SUCCESS. If a dependency or environment variable fails
 * tests return AST_MODULE_LOAD_FAILURE. If the module can not load the 
 * configuration file or other non-critical problem return 
 * AST_MODULE_LOAD_DECLINE. On success return AST_MODULE_LOAD_SUCCESS.
 */
static int load_module(void)
{
	if (load_config(0) || ast_register_application_xml(app, cpa_exec)) {
		return AST_MODULE_LOAD_DECLINE;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

static int reload(void)
{
	if (load_config(1))
		return AST_MODULE_LOAD_DECLINE;
	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "DSP Call Progress Application",
		.support_level = AST_MODULE_SUPPORT_EXTENDED,
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
);
