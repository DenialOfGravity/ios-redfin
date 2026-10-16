/*
 * Entitlements for the SWEET built-in AMFI interface (bsd/kern/sweet_amfi.c).
 *
 * Apple's AMFI keeps a CoreEntitlements context per code signature; that
 * library is closed. Here each signature with an XML entitlements blob gets
 * a SweetEntitlements object: the blob parsed into an OSDictionary (the
 * representation IOKit and the kernel KPIs hand out) plus the original XML,
 * which csops(CS_OPS_ENTITLEMENTS_BLOB) returns. The blob was already
 * checked against the code directory by csblob_get_entitlements().
 */
#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <IOKit/IOLib.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSUnserialize.h>
#include <kern/cs_blobs.h>

class SweetEntitlements : public OSObject {
	OSDeclareDefaultStructors(SweetEntitlements);

public:
	OSDictionary *dict;
	CS_GenericBlob *xml;            /* the signature's blob, header included */
	size_t xml_len;

	bool init(void) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetEntitlements, OSObject);

bool
SweetEntitlements::init(void)
{
	return OSObject::init();
}

void
SweetEntitlements::free(void)
{
	OSSafeReleaseNULL(dict);
	if (xml) {
		kfree_data(xml, xml_len);
		xml = NULL;
	}
	OSObject::free();
}

static SweetEntitlements *
sweet_ent(const void *obj)
{
	return OSDynamicCast(SweetEntitlements, (OSObject *)(uintptr_t)obj);
}

extern "C" {
/* Parsed entitlements for one XML blob (header included), or NULL. Retained. */
void *
sweet_entitlements_create(const CS_GenericBlob *blob, size_t blob_len)
{
	if (blob == NULL || blob_len <= sizeof(CS_GenericBlob) ||
	    ntohl(blob->magic) != CSMAGIC_EMBEDDED_ENTITLEMENTS ||
	    ntohl(blob->length) != blob_len) {
		return NULL;
	}

	/* OSUnserializeXML needs a terminating NUL inside the buffer. */
	size_t text_len = blob_len - sizeof(CS_GenericBlob);
	char *text = (char *)kalloc_data(text_len + 1, Z_WAITOK);
	if (text == NULL) {
		return NULL;
	}
	memcpy(text, blob->data, text_len);
	text[text_len] = '\0';
	OSObject *parsed = OSUnserializeXML(text, text_len + 1);
	kfree_data(text, text_len + 1);

	OSDictionary *dict = OSDynamicCast(OSDictionary, parsed);
	if (dict == NULL) {
		OSSafeReleaseNULL(parsed);
		return NULL;
	}

	SweetEntitlements *ent = OSTypeAlloc(SweetEntitlements);
	CS_GenericBlob *xml = (CS_GenericBlob *)kalloc_data(blob_len, Z_WAITOK);
	if (ent == NULL || !ent->init() || xml == NULL) {
		OSSafeReleaseNULL(ent);
		if (xml) {
			kfree_data(xml, blob_len);
		}
		dict->release();
		return NULL;
	}
	memcpy(xml, blob, blob_len);
	ent->dict = dict;
	ent->xml = xml;
	ent->xml_len = blob_len;
	return ent;
}

/* The entitlements as an OSDictionary. Retained. */
void *
sweet_entitlements_copy_dict(const void *obj)
{
	SweetEntitlements *ent = sweet_ent(obj);
	if (ent == NULL) {
		return NULL;
	}
	ent->dict->retain();
	return ent->dict;
}

/* A kalloc_data copy of the XML blob; the caller frees it (blob length). */
bool
sweet_entitlements_copy_xml(const void *obj, CS_GenericBlob **out)
{
	SweetEntitlements *ent = sweet_ent(obj);
	if (ent == NULL) {
		return false;
	}
	CS_GenericBlob *copy = (CS_GenericBlob *)kalloc_data(ent->xml_len, Z_WAITOK);
	if (copy == NULL) {
		return false;
	}
	memcpy(copy, ent->xml, ent->xml_len);
	*out = copy;
	return true;
}

/* name is true (a boolean entitlement). */
bool
sweet_entitlements_bool(const void *obj, const char *name)
{
	SweetEntitlements *ent = sweet_ent(obj);
	return ent != NULL && ent->dict->getObject(name) == kOSBooleanTrue;
}

/* name is the string value, or an array containing it. */
bool
sweet_entitlements_string(const void *obj, const char *name, const char *value)
{
	SweetEntitlements *ent = sweet_ent(obj);
	if (ent == NULL || value == NULL) {
		return false;
	}
	OSObject *v = ent->dict->getObject(name);
	OSString *s = OSDynamicCast(OSString, v);
	if (s != NULL) {
		return s->isEqualTo(value);
	}
	OSArray *a = OSDynamicCast(OSArray, v);
	for (unsigned int i = 0; a != NULL && i < a->getCount(); i++) {
		s = OSDynamicCast(OSString, a->getObject(i));
		if (s != NULL && s->isEqualTo(value)) {
			return true;
		}
	}
	return false;
}

/* Number of entitlements. */
unsigned int
sweet_entitlements_count(const void *obj)
{
	SweetEntitlements *ent = sweet_ent(obj);
	return ent != NULL ? ent->dict->getCount() : 0;
}

/* The value of name, or NULL. Retained. */
void *
sweet_entitlements_copy_object(const void *obj, const char *name)
{
	SweetEntitlements *ent = sweet_ent(obj);
	OSObject *v = ent != NULL ? ent->dict->getObject(name) : NULL;
	if (v != NULL) {
		v->retain();
	}
	return v;
}
}
#endif /* SWEET */
