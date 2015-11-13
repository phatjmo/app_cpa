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
					<value name="Unknown" />
					<value name="NoFrames" />
					<value name="FoundDTMF" />
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
