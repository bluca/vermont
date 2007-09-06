#include "ExporterModule.h"

ExporterModule::ExporterModule()
{
}

ExporterModule::~ExporterModule()
{
}

#if 0
ExporterSink(Template *tmpl, int sID)
	: Sink("ExporterSink"), sourceID(sID), templ(tmpl),
			numPacketsToRelease(0), numMetaFieldsToRelease(0),
			ipfix_maxrecords(MAX_RECORDS_PER_PACKET), exportTimeout(MAX_PACKET_LIFETIME),
			exitFlag(false)
 {
         int ret, i, tmplid;
         unsigned short ttype, tlength, toffset, theader;

         // generate the exporter
         ret = ipfix_init_exporter(sourceID, &exporter);
         if(ret) {
                 msg(MSG_FATAL, "error initializing IPFIX exporter");
                 exit(1);
         }

         // generate the ipfix template
         tmplid = templ->getTemplateID();
         ret =  ipfix_start_template_set(exporter, tmplid, templ->getFieldCount());

         for(i = 0; i < templ->getFieldCount(); i++) {
                 templ->getFieldInfo(i, &ttype, &tlength, &toffset, &theader);
                 ipfix_put_template_field(exporter, tmplid, ttype, tlength, 0);
         }

         ipfix_end_template_set(exporter, tmplid);
 };


~ExporterSink()
 {
         msg(MSG_DEBUG, "Sink: destructor called");
         terminateSink();
         ipfix_deinit_exporter(exporter);
 };

bool setMaxRecords(int x)
{
	ipfix_maxrecords = x;
	return true;
}

int getMaxRecords()
{
	return ipfix_maxrecords;
}

bool setExportTimeout(int ms)
{
	exportTimeout = ms;
	return true;
}

void ExporterModule::createExporterSink(Template* t, uint16_t observationDomainId, uint16_t recordLength)
{
	msg(MSG_INFO, "Creating exporter sink");
	exporterSink = new ExporterSink(t, observationDomainId);
	if(recordLength || maxPacketSize)
	{
	    // IPFIX packet header: 16 bytes, set header: 4 bytes
	    int recordsPerPacket = (maxPacketSize - 16 - 4) / recordLength;
	    if(recordsPerPacket <= 0) recordsPerPacket = 1;
	    msg(MSG_INFO, "Set maximum records per packet to %d", recordsPerPacket);
	    exporterSink->setMaxRecords(recordsPerPacket);
	}
	if(exportDelay)
	{
	    msg(MSG_INFO, "Set maximum export timeout to %d", exportDelay);
	    exporterSink->setExportTimeout(exportDelay);
	}
	if(templateRefreshTime || templateRefreshRate)
	{
	    msg(MSG_ERROR, "Exporter: Configuration of templateRefreshRate/Time not yet supported.");
	}
	for (unsigned i = 0; i != collectors.size(); ++i) {
		msg(MSG_DEBUG, "Exporter: adding collector %s:%d to ExporterSink",
		    collectors[i]->ipAddress.c_str(),
		    collectors[i]->port);
		exporterSink->addCollector(collectors[i]->ipAddress.c_str(),
					   collectors[i]->port,
					   collectors[i]->protocolType.c_str());
	}
}

#include <sys/time.h>
#include <time.h>

#include "common/msg.h"
#include "ipfixlolib/ipfixlolib.h"

#include "ExporterSink.h"

using namespace std;


// the usage of the following functions is:
//
// StartPacketStream();
// while (more_packets AND timeout_not_met)
// {
//   AddPaket(getNextPacket());
// }
// FlushPacketStream();
//
// This schema is realized in exporterSinkProcess

// start a new IPFIX packet stream
void ExporterSink::startNewPacketStream()
{
    unsigned short net_tmplid = htons(templ->getTemplateID());
    DPRINTF("Starting new packet stream");
    ipfix_start_data_set(exporter, net_tmplid);
}

// Add this packet to the packet stream
// returns true, if the Packet was successfully added
bool ExporterSink::addPacket(Packet *pck)
{
	unsigned short ttype, tlength, toffset, theader;
	unsigned short enc_length = 0;
	void *data, *metadata;
	uint8_t *enc_value = 0;

	// first check, if we can buffer this packet
	if (!(numPacketsToRelease < MAX_PACKETS)) {
		msg(MSG_ERROR, "packet buffer too small, packet dropped.");
		DPRINTF("dropping packet");
		pck->removeReference();
		return false;
	}

	// now, check if packet matches template requirements, i.e. if all fields are available
	if (!templ->checkPacketConformity(pck->classification)) {
		DPRINTF("Packet does not contain all fields required by the template! Skip this packet.");
		goto error2;
	}

	// store current position in order to go back in case of problems
	ipfix_set_data_field_marker(exporter);

	for (int i = 0; i < templ->getFieldCount(); i++) {
		templ->getFieldInfo(i, &ttype, &tlength, &toffset, &theader);
		if (ttype >= 0x8000) {
			// it is a meta-field --> get the metadata
			metadata = templ->getMetaFieldData(i);
			ipfix_put_data_field(exporter, metadata, tlength);
			metaFieldsToRelease[numMetaFieldsToRelease++] = metadata;
		} else if (ttype == IPFIX_TYPEID_flowStartSeconds) {
			ipfix_put_data_field(exporter, &(pck->time_sec_nbo), tlength);
		} else if (ttype == IPFIX_TYPEID_flowStartMilliSeconds) {
			ipfix_put_data_field(exporter, &(pck->time_msec_nbo), tlength);
		} else if (ttype == IPFIX_TYPEID_flowStartMicroSeconds) {
			ipfix_put_data_field(exporter, &(pck->time_usec_nbo), tlength);
		} else if (tlength == 65535) {
			// variable length field
			data = pck->getVariableLengthPacketData(&tlength, &enc_value, &enc_length, toffset, theader);
			if(data == NULL) {
				msg(MSG_ERROR, "ExporterSink: getVariableLengthPacketData returned NULL! This should never happen!");
				goto error1;
			}
			// put the length information first
			ipfix_put_data_field(exporter, enc_value, enc_length);
			ipfix_put_data_field(exporter, data, tlength);
		} else {
			// check if getPacketData actually returns data
			// Note: getPacketData checks if data of size tlength is available.
			//       if not, it returns NULL
			data = pck->getPacketData(toffset, theader, tlength);
			if(data == NULL) {
				msg(MSG_ERROR, "ExporterSink: getPacketData returned NULL! packet length or pcap capture length is too small.");
				goto error1;
			}
			ipfix_put_data_field(exporter, data, tlength);
		}
	}

	// if we will export the packet, we keep and and release it later, after we have sent the data
	DPRINTF("Adding packet to buffer");
	packetsToRelease[numPacketsToRelease++] = pck;
	return true;

error1:
	// delete the fields that we have already added
	ipfix_delete_data_fields_upto_marker(exporter);

error2:
	// we do no export this packet, i.e. we can release it right now.
	DPRINTF("dropping packet");
	pck->removeReference();
	return false;
}

// send out the IPFIX packet stream and reset
void ExporterSink::flushPacketStream() {
	// end the packet stream and send the IPFIX packet out through the wire
	ipfix_end_data_set(exporter);
	ipfix_send(exporter);

	DPRINTF("dropping %d packets", numPacketsToRelease);
	for (int i = 0; i < numPacketsToRelease; i++) {
		(packetsToRelease[i])->removeReference();
	}
	// now release the additional metadata fields
	DPRINTF("Flushing %d Metadata fields from buffer",
			numMetaFieldsToRelease);
	for (int i = 0; i < numMetaFieldsToRelease; i++) {
		free(metaFieldsToRelease[i]);
	}
	numPacketsToRelease = 0;
	numMetaFieldsToRelease = 0;
}

// this runs in the thread
void *ExporterModule::exporterSinkProcess(void *arg) {
	ExporterSink *sink = (ExporterSink *)arg;
	ConcurrentQueue<Packet*> *queue = sink->getQueue();
	Packet *p;
	bool result;
	// our deadline
	struct timeval deadline;
	int pckCount;

	msg(MSG_INFO, "Sink: now running ExporterSink thread");

	while (!sink->exitFlag) {
		sink->startNewPacketStream();

		// let's get the first packet
		gettimeofday(&deadline, 0);

		result = queue->pop(&p);

		if (result == true) {
			// we got a packet, so let's add the record
			result = sink->addPacket(p);
		}

		pckCount = 1;

		// now calculate the deadline by which the packet has to leave the exporter
		gettimeofday(&deadline, 0);
		deadline.tv_usec += sink->exportTimeout * 1000L;
		if (deadline.tv_usec > 1000000L) {
			deadline.tv_sec += (deadline.tv_usec / 1000000L);
			deadline.tv_usec %= 1000000L;
		}

		while (!sink->exitFlag && (pckCount < sink->ipfix_maxrecords)) {
			// Try to get next packet from queue before our deadline
			result = queue->popAbs(deadline, &p);

			// check for timeout and break loop if neccessary
			if (!result)
				break;

			// no timeout received, continue waiting, but
			// count only if packet was added
			if (sink->addPacket(p) == true)
				pckCount++;
		}
		sink->flushPacketStream();
	}
	return 0;
}


bool ExporterSink::addCollector(const char *address, unsigned short port, const char *protocol)
{
	ipfix_transport_protocol proto;

	if(strcasecmp(protocol, "TCP") == 0) {
		proto = TCP;
	} else if(strcasecmp(protocol, "UDP") == 0) {
		proto = UDP;
	} else {
		msg(MSG_ERROR, "ExporterSink: invalid protocol %s for %s",
		    protocol, address);
		return false;
	}

	DPRINTF("Adding %s://%s:%d", protocol, address, port);
	return(ipfix_add_collector(exporter, address, port, proto) == 0);
}
#endif // #if 0