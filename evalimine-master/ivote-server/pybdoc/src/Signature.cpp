/*
 * Copyright: Eesti Vabariigi Valimiskomisjon
 * (Estonian National Electoral Committee), www.vvk.ee
 * Derived work from libdicidocpp library
 * https://svn.eesti.ee/projektid/idkaart_public/trunk/libdigidocpp/
 * Written in 2011-2013 by Cybernetica AS, www.cyber.ee
 *
 * This work is licensed under the Creative Commons
 * Attribution-NonCommercial-NoDerivs 3.0 Unported License.
 * To view a copy of this license, visit
 * http://creativecommons.org/licenses/by-nc-nd/3.0/.
 * */

#include "Signature.h"
#include "crypto/X509Cert.h"
#include "crypto/X509CertStore.h"
#include "crypto/Digest.h"
#include "DateTime.h"
#include "xml/xmldsig-core-schema.hxx"
#include "xml/XAdES.hxx"
#include <xercesc/dom/DOM.hpp>
#include <xsec/canon/XSECC14n20010315.hpp>
#include <xercesc/parsers/XercesDOMParser.hpp>
#include <xercesc/framework/MemBufInputSource.hpp>
#include <xercesc/framework/StdOutFormatTarget.hpp>
#include <xercesc/util/XMLString.hpp>
#include <xsec/dsig/DSIGConstants.hpp>
#include <xsec/utils/XSECSafeBuffer.hpp>
#include "BDoc.h"
#include "StackException.h"
#include "XMLHelper.h"

const std::string bdoc::XAdES111Signature::XADES111_NAMESPACE =
					"http://uri.etsi.org/01903/v1.1.1#";

const std::string bdoc::XAdES132Signature::XADES132_NAMESPACE =
					"http://uri.etsi.org/01903/v1.3.2#";

const std::string bdoc::Signature::DSIG_NAMESPACE =
					"http://www.w3.org/2000/09/xmldsig#";


std::string serializeDOM(xercesc::DOMNode* node) {

	std::string c14n;
	XSECC14n20010315 canonicalizer(node->getOwnerDocument(), node);
	canonicalizer.setCommentsProcessing(false);
	canonicalizer.setUseNamespaceStack(true);

	unsigned char buffer[1024];
	int bytes = 0;
	while ((bytes = canonicalizer.outputBuffer(buffer, 1024)) > 0) {
		for (int i = 0; i < bytes; i++) {
			c14n += (char)buffer[i];
		}
	}

	return c14n;
}

/*
 *
 * Class SignatureValidator
 *
 * */

bdoc::SignatureValidator::SignatureValidator(bdoc::Signature *sig,
						bdoc::Configuration *cf) :
	_sig(sig),
	_conf(cf),
	_signingCert(),
	_ocspCerts(NULL),
	_issuerX509(NULL),
	_ocspResponse(),
	_producedAt()
{
}

bdoc::SignatureValidator::~SignatureValidator()
{
	if (_ocspCerts) {
		sk_X509_pop_free(_ocspCerts, X509_free);
	}
	if (_issuerX509) {
		X509_free(_issuerX509);
	}
}

std::string bdoc::SignatureValidator::getProducedAt() const
{
	return bdoc::util::date::xsd2string(
		bdoc::util::date::makeDateTime(_producedAt));
}

bdoc::OCSP* bdoc::SignatureValidator::prepare()
{
	_signingCert = _sig->getSigningCertificate();

	std::string issuer = _signingCert.getIssuerName();
	int pos = issuer.find("CN=", 0) + 3;
	std::string issure_cn =
			issuer.substr(pos, issuer.find(",", pos) - pos);
	if (!_conf->hasOCSPConf(issure_cn)) {
		THROW_STACK_EXCEPTION("Failed to find ocsp responder.");
	}

	OCSPConf ocspConf = _conf->getOCSPConf(issure_cn);

	_issuerX509 = _conf->getCertStore()->
			getCert(*(_signingCert.getIssuerNameAsn1()));
	if (_issuerX509 == NULL) {
		THROW_STACK_EXCEPTION("Failed to load issuer certificate.");
	}

	_ocspCerts = X509Cert::loadX509Stack(ocspConf.cert);

	OCSP *ocsp = new OCSP(ocspConf.url);
	ocsp->setSkew(ocspConf.skew);
	ocsp->setMaxAge(ocspConf.maxAge);
	ocsp->setOCSPCerts(_ocspCerts);

	return ocsp;
}

bdoc::OCSP::CertStatus bdoc::SignatureValidator::validateBESOnline()
{
	std::auto_ptr<OCSP> ocsp(prepare());
	std::auto_ptr<Digest> sigCalc = Digest::create(_conf->getDigestURI());
	sigCalc->update(_sig->getSignatureValue());

	return ocsp->checkCert(_signingCert.getX509(),
				_issuerX509,
				sigCalc->getDigest(),
				_ocspResponse,
				_producedAt);
}

std::string bdoc::SignatureValidator::getTMSignature()
{
	std::string ret;

	X509Cert ocspCert(sk_X509_value(_ocspCerts, 0));

	std::auto_ptr<Digest> ocspResponseCalc =
					Digest::create(_conf->getDigestURI());

	ocspResponseCalc->update(_ocspResponse);
	std::vector<unsigned char>
			ocspResponseHash = ocspResponseCalc->getDigest();

	std::auto_ptr<xercesc::DOMDocument> doc = _sig->createDom();
	xercesc::DOMNodeList *nl =
		doc->getElementsByTagNameNS(
			xercesc::XMLString::transcode("*"),
			xercesc::XMLString::transcode("UnsignedProperties"));

	xercesc::DOMNode *unsignedprops = nl->item(0);
	xercesc::DOMNode *unsignedsignatureprops =
		doc->createElement(
			xercesc::XMLString::transcode(
					"UnsignedSignatureProperties"));

	unsignedprops->appendChild(unsignedsignatureprops);

	{
		X509Cert issuerCert(_issuerX509);
		addXMLCertificateValues(doc.get(), unsignedsignatureprops,
					ocspCert, issuerCert);
	}

	{
		xml_schema::Base64Binary resp(&_ocspResponse[0],
						_ocspResponse.size());
		addXMLRevocationValues(doc.get(), unsignedsignatureprops,
					resp);
	}

	{
		X509* ocspIssuerCert =
			_conf->getCertStore()->getCert(
					*(ocspCert.getIssuerNameAsn1()));

		X509_scope ocspIssuerCertScope(&ocspIssuerCert);
		if (ocspIssuerCert == NULL) {
			THROW_STACK_EXCEPTION(
				"Failed to load issuer certificate.");
		}

		X509Cert oic(ocspIssuerCert);
		std::vector<unsigned char> der = oic.encodeDER();
		std::string oicmeth(_conf->getDigestURI());
		std::auto_ptr<bdoc::Digest> oicCalc =
					bdoc::Digest::create(oicmeth);
		oicCalc->update(der);
		xml_schema::Base64Binary oicDig(&oicCalc->getDigest()[0],
						oicCalc->getSize());

		addXMLCompleteCertificateRefs(doc.get(),
						unsignedsignatureprops,
						oic, oicDig, oicmeth);
	}

	{
		xml_schema::Base64Binary oh(&ocspResponseHash[0],
						ocspResponseHash.size());

		addXMLCompleteRevocationRefs(
				doc.get(), unsignedsignatureprops,
				ocspCert, oh, ocspResponseCalc->getUri(),
				bdoc::util::date::xsd2string(
					bdoc::util::date::
						makeDateTime(_producedAt)));
	}

	xercesc::DOMElement* root (doc->getDocumentElement ());
	ret = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
	ret += serializeDOM(root);
	return ret;
}

void bdoc::SignatureValidator::validateTMOffline()
{

//	   1. Check OCSP response (RevocationValues) was signed by OCSP server
//	   2. OCSP server certificate is trusted?
//	   3. Check that nonce field in OCSP response is same as
//					CompleteRevocationRefs->DigestValue
//	   4. Recalculate hash of signature and compare with nonce

	std::auto_ptr<OCSP> ocsp(prepare());

	_sig->getOCSPResponseValue(_ocspResponse);
	ocsp->verifyResponse(_ocspResponse);

	std::vector<unsigned char> respNonce = ocsp->getNonce(_ocspResponse);

	xml_schema::Uri method = _sig->ocspDigestAlgorithm();

	std::auto_ptr<Digest> calc = Digest::create(std::string(method));
	calc->update(_sig->getSignatureValue());
	std::vector<unsigned char> nonce = calc->getDigest();

	if (nonce != respNonce) {
		THROW_STACK_EXCEPTION(
			"Calculated signature hash doesn't match to OCSP "
			"responder nonce field");
	}

	std::vector<unsigned char> revocationOCSPRefValue(0);
	std::string ocspResponseHashUri;
	_sig->getRevocationOCSPRef(revocationOCSPRefValue,
					ocspResponseHashUri);

	std::auto_ptr<Digest> ocspResponseCalc =
					Digest::create(ocspResponseHashUri);

	ocspResponseCalc->update(_ocspResponse);
	std::vector<unsigned char> ocspResponseHash =
					ocspResponseCalc->getDigest();

	if (ocspResponseHash != revocationOCSPRefValue) {
		THROW_STACK_EXCEPTION(
			"OCSPRef value doesn't match with hash of OCSP "
			"response");
	}
}


/*
 *
 * Class Signature
 *
 * */

bdoc::Signature* bdoc::Signature::parse(const std::string& schema_dir,
					const char *xml_buf, size_t buf_len, ContainerInfo *ci)
{
	try {
		xml_schema::Properties properties;
		properties.schema_location(bdoc::XAdES111Signature::XADES111_NAMESPACE, schema_dir + "/XAdES111.xsd");
		properties.schema_location(bdoc::XAdES132Signature::XADES132_NAMESPACE, schema_dir + "/XAdES.xsd");
		properties.schema_location(DSIG_NAMESPACE, schema_dir + "/xmldsig-core-schema.xsd");
		std::istringstream input(std::string(xml_buf, buf_len));
		xml_schema::Flags flags = xml_schema::Flags::dont_initialize;
		std::auto_ptr<dsig::SignatureType> sig(dsig::signature(input, flags, properties).release());

		dsig::SignatureType::ObjectSequence& os = sig->object();
		if (os.empty()) {
			THROW_STACK_EXCEPTION("Signature block 'Object' is missing.");
		}
		else if (os.size() != 1) {
			THROW_STACK_EXCEPTION(
				"Signature block contains more than one "
				"'Object' block.");
		}
		dsig::ObjectType& o = os[0];

		dsig::ObjectType::QualifyingPropertiesSequence& qpSeq = o.qualifyingProperties();
		dsig::ObjectType::QualifyingProperties1Sequence& qp1Seq = o.qualifyingProperties1();

		if (qpSeq.empty() && qp1Seq.empty()) {
			THROW_STACK_EXCEPTION("Signature block 'QualifyingProperties' is missing.");
		}

		if (qpSeq.empty() && (!qp1Seq.empty())) {
			if (qp1Seq.size() != 1) {
				THROW_STACK_EXCEPTION(
					"Signature block 'Object' contains more than one "
					"'QualifyingProperties' block.");
			}
			return new XAdES111Signature(sig.release(), xml_buf, buf_len, ci);
		}

		if ((!qpSeq.empty()) && qp1Seq.empty()) {
			if (qpSeq.size() != 1) {
				THROW_STACK_EXCEPTION(
					"Signature block 'Object' contains more than one "
					"'QualifyingProperties' block.");
			}
			return new XAdES132Signature(sig.release(), xml_buf, buf_len, ci);
		}

		THROW_STACK_EXCEPTION("Signature block 'Object' contains more than one 'QualifyingProperties' block.");
	}
	catch (const xml_schema::Parsing& e) {
		std::ostringstream oss;
		oss << e;
		THROW_STACK_EXCEPTION(
			"Failed to parse signature XML: %s",
			oss.str().c_str());
	}
	catch (const xsd::cxx::exception& e) {
		THROW_STACK_EXCEPTION(
			"Failed to parse signature XML: %s", e.what());
	}
}




bdoc::Signature::Signature(dsig::SignatureType* signature, const char *xml, size_t xml_len, ContainerInfo *ci)
	: _sign(signature), _xml(xml), _xml_len(xml_len), _bdoc(ci)
{
}

bdoc::Signature::~Signature()
{
	delete _sign;
}

void bdoc::Signature::validateOffline(bdoc::X509CertStore *store)
{
	DECLARE_STACK_EXCEPTION("Signature is invalid");

	try {
		checkQualifyingProperties();
	}
	catch (StackExceptionBase& e) {
		exc.add(e);
	}

	try {
		checkSignatureMethod();
		checkReferences();
		checkKeyInfo();
		checkSignatureValue();
	}
	catch (StackExceptionBase& e) {
		exc.add(e);
	}

	try {
		checkSigningCertificate(store);
	}
	catch (StackExceptionBase& e) {
		exc.add(e);
	}

	if (exc.hasCauses()) {
		throw exc;
	}
}

std::string bdoc::Signature::getSubject() const
{
	return getSigningCertificate().getSubject();
}

std::vector<unsigned char> bdoc::Signature::getSignatureValue() const
{
	dsig::SignatureType::SignatureValueType
		signatureValueType = _sign->signatureValue();

	std::vector<unsigned char>
		signatureValue(signatureValueType.size(), 0);

	memcpy(&signatureValue[0],
		signatureValueType.data(), signatureValueType.size());

	return signatureValue;
}

std::vector<unsigned char> bdoc::Signature::calcDigestOnNode(
		Digest* calc, const std::string& ns,
		const std::string& tagName)
{
	// Parse Xerces DOM from file, to preserve the white spaces "as is"
	// and get the same digest value on XML node.
	// Canonical XML 1.0 specification
	// (http://www.w3.org/TR/2001/REC-xml-c14n-20010315)
	// needs all the white spaces from XML file "as is", otherwise the
	// digests won't match. Therefore we have to use Xerces to parse the
	// XML file each time a digest needs to be calculated on a XML node.
	// If you are parsing XML files with a parser that doesn't preserve
	// the white spaces you are DOOMED!
	std::auto_ptr<xercesc::DOMDocument> dom = createDom();

	// Select node, on which the digest is calculated.
	XMLCh* tagNs(xercesc::XMLString::transcode(ns.c_str()));
	XMLCh* tag(xercesc::XMLString::transcode(tagName.c_str()));
	xercesc::DOMNodeList* nodeList =
		dom->getElementsByTagNameNS(tagNs, tag);

	xercesc::XMLString::release(&tagNs);
	xercesc::XMLString::release(&tag);

	if ((nodeList == NULL) || (nodeList->getLength() < 1)) {
		THROW_STACK_EXCEPTION(
			"Could not find '%s' node which is in '%s' namespace "
			"in signature XML.", tagName.c_str(), ns.c_str());
	}

	if (nodeList->getLength() > 1) {
		THROW_STACK_EXCEPTION(
			"Found %d '%s' nodes which are in '%s' namespace in "
			"signature XML, can not calculate digest on XML node.",
			nodeList->getLength(), tagName.c_str(), ns.c_str());
	}

	// Canocalize XML using one of the three methods supported by XML-DSIG
	XSECC14n20010315 canonicalizer(dom.get(), nodeList->item(0));
	canonicalizer.setCommentsProcessing(false);
	canonicalizer.setUseNamespaceStack(true);

	// Find the method identifier
	dsig::SignedInfoType& signedInfo = _sign->signedInfo();
	dsig::CanonicalizationMethodType&
		canonMethod = signedInfo.canonicalizationMethod();

	dsig::CanonicalizationMethodType::AlgorithmType&
		algorithmType = canonMethod.algorithm();

	// Set processing flags according to algorithm type.
	if (algorithmType == URI_ID_C14N_NOC) {
		// Default behaviour, nothing needs to be changed
	}
	else if (algorithmType == URI_ID_C14N_COM) {
		canonicalizer.setCommentsProcessing(true);
	}
	else if (algorithmType == URI_ID_EXC_C14N_NOC) {
	// Exclusive mode needs to include xml-dsig in root element
	// in order to maintain compatibility with existing implementations
		canonicalizer.setExclusive((char*)"ds");
#ifdef URI_ID_C14N11_NOC
	}
	else if (algorithmType == URI_ID_C14N11_NOC) {
		canonicalizer.setInclusive11();
	}
	else if (algorithmType == URI_ID_C14N11_COM) {
		canonicalizer.setInclusive11();
		canonicalizer.setCommentsProcessing(true);
#endif
	}
	else {
		THROW_STACK_EXCEPTION(
			"Unsupported SignedInfo canonicalization method '%s'",
			algorithmType.c_str());
	}

//	std::string out;

	unsigned char buffer[1024];
	int bytes = 0;
	while ((bytes = canonicalizer.outputBuffer(buffer, 1024)) > 0) {
//		out += std::string((char *)buffer, bytes);
		calc->update(buffer, bytes);
	}
//	std::cout << out << std::endl;

	return calc->getDigest();
}

std::auto_ptr<xercesc::DOMDocument> bdoc::Signature::createDom() const
{

	try {
		std::auto_ptr<xercesc::XercesDOMParser>
			parser(new xercesc::XercesDOMParser());

		parser->setValidationScheme(
			xercesc::XercesDOMParser::Val_Always);

		parser->setDoNamespaces(true);

		xercesc::MemBufInputSource memIS((const XMLByte*)_xml, _xml_len, "test", false);

		parser->parse(memIS);
		xercesc::DOMNode* dom = parser->getDocument()->cloneNode(true);

		return std::auto_ptr<xercesc::DOMDocument>
				(static_cast<xercesc::DOMDocument*>(dom));
	}
	catch (const xercesc::XMLException& e) {
		char* tmp = xercesc::XMLString::transcode(e.getMessage());
		std::string msg(tmp);
		xercesc::XMLString::release(&tmp);
		THROW_STACK_EXCEPTION(
			"Failed to parse signature XML: %s", msg.c_str());
	}
	catch (const xercesc::DOMException& e) {
		char* tmp = xercesc::XMLString::transcode(e.msg);
		std::string msg(tmp);
		xercesc::XMLString::release(&tmp);
		THROW_STACK_EXCEPTION(
			"Failed to parse signature XML: %s", msg.c_str());
	}
	catch (...) {
		THROW_STACK_EXCEPTION("Failed to parse signature XML.");
	}
	return std::auto_ptr<xercesc::DOMDocument>(NULL);
}

bdoc::X509Cert bdoc::Signature::getSigningCertificate() const
{
	const dsig::X509DataType::X509CertificateType&
		certBlock = getSigningX509CertificateType();

	return X509Cert(
		(const unsigned char*)certBlock.data(), certBlock.size());
}

bdoc::dsig::X509DataType::X509CertificateType&
	bdoc::Signature::getSigningX509CertificateType() const
{
	dsig::SignatureType::KeyInfoOptional&
		keyInfoOptional = _sign->keyInfo();
	if (!keyInfoOptional.present()) {
		THROW_STACK_EXCEPTION(
			"Signature does not contain signer certificate");
	}

	dsig::KeyInfoType& keyInfo = keyInfoOptional.get();

	dsig::KeyInfoType::X509DataSequence& x509DataSeq = keyInfo.x509Data();
	if (x509DataSeq.empty()) {
		THROW_STACK_EXCEPTION(
			"Signature does not contain signer certificate");
	}
	else if (x509DataSeq.size() != 1) {
		THROW_STACK_EXCEPTION(
			"Signature contains more than one signer certificate");
	}
	dsig::X509DataType& x509Data = x509DataSeq.front();

	dsig::X509DataType::X509CertificateSequence&
		x509CertSeq = x509Data.x509Certificate();
	if (x509CertSeq.empty()) {
		THROW_STACK_EXCEPTION(
			"Signature does not contain signer certificate");
	}
	else if (x509CertSeq.size() != 1) {
		THROW_STACK_EXCEPTION(
			"Signature contains more than one signer certificate");
	}
	dsig::X509DataType::X509CertificateType&
		certBase64Buf = x509CertSeq.front();

	return certBase64Buf;
}

void bdoc::Signature::validateIdentifier() const
{
	const dsig::SignatureType::IdOptional& idOptional = _sign->id();
	if (!idOptional.present()) {
		THROW_STACK_EXCEPTION(
			"Signature element mandatory attribute "
			"'Id' is missing");
	}

	if (idOptional.get().empty()) {
		THROW_STACK_EXCEPTION(
			"Signature element mandatory attribute 'Id' is empty");
	}
}

bdoc::dsig::KeyInfoType& bdoc::Signature::keyInfo() const
{
	dsig::SignatureType::KeyInfoOptional&
		keyInfoOptional = _sign->keyInfo();
	if (!keyInfoOptional.present()) {
		THROW_STACK_EXCEPTION(
			"Signature mandatory element KeyInfo is missing");
	}

	return keyInfoOptional.get();
}

bdoc::dsig::SignatureMethodType::AlgorithmType&
	bdoc::Signature::getSignatureMethodAlgorithmType() const
{
	dsig::SignedInfoType& signedInfo = _sign->signedInfo();
	dsig::SignatureMethodType& sigMethod = signedInfo.signatureMethod();
	return sigMethod.algorithm();
}

void bdoc::Signature::checkSignatureMethod() const
{
	dsig::SignatureMethodType::AlgorithmType&
		algorithmType = getSignatureMethodAlgorithmType();
	if (algorithmType != URI_ID_RSA_SHA1
			&& algorithmType != URI_ID_RSA_SHA224
			&& algorithmType != URI_ID_RSA_SHA256) {
		THROW_STACK_EXCEPTION(
			"Unsupported SignedInfo signature method \"%s\"",
			algorithmType.c_str());
	}
}

void bdoc::Signature::checkReferences()
{
	dsig::SignedInfoType& signedInfo = _sign->signedInfo();
	dsig::SignedInfoType::ReferenceSequence&
		refSeq = signedInfo.reference();

	if (refSeq.size() != (_bdoc->documentCount() + 1)) {
		// we require exactly one ref to every document,
		// plus one ref to the SignedProperties
		THROW_STACK_EXCEPTION(
			"Number of references in SignedInfo is invalid: "
			"found %d, expected %d",
			refSeq.size(), _bdoc->documentCount() + 1);
	}

	bool gotSignatureRef = false;
	for (dsig::SignedInfoType::ReferenceSequence::const_iterator
		itRef = refSeq.begin(); itRef != refSeq.end(); itRef++) {

		const dsig::ReferenceType& refType = (*itRef);

		if (isReferenceToSigProps(refType)) {
			// the one and only reference to SignedProperties
			if (gotSignatureRef) {
				THROW_STACK_EXCEPTION(
					"SignedInfo element refers to more "
					"than one SignedProperties");
			}
			gotSignatureRef = true;
			checkReferenceToSigProps(refType);
		}
	}

	if (!gotSignatureRef) {
		THROW_STACK_EXCEPTION(
			"SignedInfo does not contain reference to "
			"SignedProperties");
	}

	checkReferencesToDocs(refSeq);
}

void bdoc::Signature::checkSigningCertificate(bdoc::X509CertStore *store) const
{
	X509Cert signingCert = getSigningCertificate();

	if (store == NULL) {
		THROW_STACK_EXCEPTION(
			"Unable to verify signing certificate %s",
			signingCert.getSubject().c_str());
	}
	X509_STORE *st = NULL;
	st = store->getCertStore();

	int res = signingCert.verify(st);

	X509_STORE_free(st);
	st = NULL;

	if (!res) {
		THROW_STACK_EXCEPTION(
			"Unable to verify signing certificate %s",
			signingCert.getSubject().c_str());
	}

}

bool bdoc::Signature::isReferenceToSigProps(
	const bdoc::dsig::ReferenceType& refType) const
{
	const dsig::ReferenceType::TypeOptional& typeOpt = refType.type();

	if (typeOpt.present()) {
		std::string typeAttr = typeOpt.get();
		//BDOC-1.0 spec says that value must be
		//"http://uri.etsi.org/01903#SignedProperties",
		//but Xades wants value in format
		//http://uri.etsi.org/01903/vX.Y.Z/#SignedProperties,
		//where  X.Y.Z is Xades version
		//Try to support all possible values

		if ((typeAttr.find("http://uri.etsi.org/01903") == 0)
			&& (typeAttr.rfind("#SignedProperties") ==
				(typeAttr.length() -
				std::string("#SignedProperties").length()))) {
			return true;
		}
	}
	return false;
}

void bdoc::Signature::checkReferenceToSigProps(
	const bdoc::dsig::ReferenceType& refType)
{
	const dsig::ReferenceType::URIOptional& uriOpt = refType.uRI();

	if (!uriOpt.present()) {
		THROW_STACK_EXCEPTION(
			"SignedInfo reference to SignedProperties does not "
			"have attribute 'URI'");
	}

	const dsig::DigestMethodType& digestMethod = refType.digestMethod();
	const dsig::DigestMethodType::AlgorithmType&
		algorithm = digestMethod.algorithm();

	if (!Digest::isSupported(algorithm)) {
		THROW_STACK_EXCEPTION(
			"reference to SignedProperties digest method "
			"algorithm '%s' is not supported", algorithm.c_str());
	}

	const dsig::DigestValueType& digestValue = refType.digestValue();

	std::auto_ptr<Digest> calc =
		Digest::create(refType.digestMethod().algorithm());

	std::vector<unsigned char> calculatedDigestValue =
		calcDigestOnNode(
			calc.get(), xadesnamespace(), "SignedProperties");

	if (digestValue.begin() + calculatedDigestValue.size()
		!= digestValue.end()) {
		THROW_STACK_EXCEPTION(
			"SignedProperties digest lengths do not match");
	}

	for (size_t i = 0; i < calculatedDigestValue.size(); i++) {
		const char* dv = digestValue.begin() + i;
		if (*dv != static_cast<char>(calculatedDigestValue[i])) {
			THROW_STACK_EXCEPTION(
				"SignedProperties digest values do not match");
		}
	}
}

void bdoc::Signature::checkReferencesToDocs(
	dsig::SignedInfoType::ReferenceSequence& refSeq) const
{
	_bdoc->checkDocumentsBegin();

	for (dsig::SignedInfoType::ReferenceSequence::const_iterator
		itRef = refSeq.begin(); itRef != refSeq.end(); itRef++) {

		const dsig::ReferenceType& refType = (*itRef);

		if (!isReferenceToSigProps(refType)) {
			const dsig::ReferenceType::URIOptional&
				uriOpt = refType.uRI();
			if (!uriOpt.present()) {
				THROW_STACK_EXCEPTION(
					"Document reference is missing "
					"attribute 'URI'");
			}
			std::string docRefUri(uriOpt.get());

			// file names in manifest do not have '/' at front
			if (!docRefUri.empty() && docRefUri[0] == '/') {
				docRefUri.erase(0, 1);
			}

			const dsig::DigestMethodType&
				digestMethod = refType.digestMethod();

			const dsig::DigestMethodType::AlgorithmType&
				algorithmType = digestMethod.algorithm();

			const dsig::DigestValueType&
				digestValueType = refType.digestValue();

			_bdoc->checkDocument(
				docRefUri, algorithmType, digestValueType);
		}
	}

	if (!_bdoc->checkDocumentsResult()) {
		THROW_STACK_EXCEPTION("Document references didn't match");
	}
}

void bdoc::Signature::checkSignatureValue()
{
	X509Cert cert(getSigningCertificate());

	const dsig::SignatureMethodType::AlgorithmType&
		algorithmType = getSignatureMethodAlgorithmType();
	const char* algorithmUri = algorithmType.c_str();

	// Get hash method URI from signature method URI.
	signatureMethod sm;
	hashMethod hm;
	safeBuffer hashMethodUri;
	if (!XSECmapURIToSignatureMethods(XMLString::transcode(algorithmUri), sm, hm)
			|| !hashMethod2URI(hashMethodUri, hm)) {
		THROW_STACK_EXCEPTION("Couldn't extract hash method from "
			"signature method URI '%s'.", algorithmUri);
	}

	std::auto_ptr<Digest> calc = Digest::create(hashMethodUri.rawCharBuffer());
	std::vector<unsigned char> digest =
		calcDigestOnNode(calc.get(), DSIG_NAMESPACE, "SignedInfo");

	std::vector<unsigned char> signatureValue = getSignatureValue();

	if (!cert.verifySignature(calc->getMethod(), calc->getSize(), digest,
														signatureValue)) {
		THROW_STACK_EXCEPTION("Signature is not valid.");
	}
}


/*
 *
 * XAdES111Signature
 *
 * */


bdoc::XAdES111Signature::XAdES111Signature(
					dsig::SignatureType* signature,
					const char *xml, size_t xml_len,
					bdoc::ContainerInfo *bdoc) : bdoc::Signature(signature, xml, xml_len, bdoc)
{
}

bdoc::XAdES111Signature::~XAdES111Signature()
{
}

const std::string& bdoc::XAdES111Signature::xadesnamespace()
{
	return XADES111_NAMESPACE;
}

bdoc::xades111::UnsignedPropertiesType::UnsignedSignaturePropertiesOptional&
bdoc::XAdES111Signature::unsignSigProps() const
{
	if (!_sign->object()[0].qualifyingProperties1()[0].
					unsignedProperties().present()) {
		THROW_STACK_EXCEPTION("Missing UnsignedProperties");
	}

	return _sign->object()[0].qualifyingProperties1()[0].
				unsignedProperties()->
						unsignedSignatureProperties();
}

void bdoc::XAdES111Signature::getOCSPResponseValue(std::vector<unsigned char>& data) const
{
	if (!unsignSigProps().present()) {
		THROW_STACK_EXCEPTION("Unsigned signature properties missing");
	}

	if (!unsignSigProps()->revocationValues().present()) {
		THROW_STACK_EXCEPTION("Revocation values missing");
	}

	xades111::RevocationValuesType t = unsignSigProps()->revocationValues().get();

	xades111::OCSPValuesType tt = t.oCSPValues().get();
	xades111::OCSPValuesType::EncapsulatedOCSPValueType resp = tt.encapsulatedOCSPValue()[0];

	data.resize(resp.size());
	std::copy(resp.data(), resp.data()+resp.size(), data.begin());
}

std::string bdoc::XAdES111Signature::getProducedAt() const
{
	if (unsignSigProps().present()) {
		const xades111::OCSPIdentifierType::ProducedAtType &producedAt =
			unsignSigProps()->completeRevocationRefs().get().oCSPRefs()->oCSPRef()[0].oCSPIdentifier().producedAt();
		return util::date::xsd2string(producedAt);
	}
	return "";
}

xml_schema::Uri bdoc::XAdES111Signature::ocspDigestAlgorithm() const
{
	return unsignSigProps()->
			completeRevocationRefs().get().oCSPRefs()->
				oCSPRef()[0].digestAlgAndValue()->
					digestMethod().algorithm();
}


void bdoc::XAdES111Signature::getRevocationOCSPRef(
	std::vector<unsigned char>& data, std::string& digestMethodUri) const
{
	xades111::UnsignedSignaturePropertiesType::CompleteRevocationRefsOptional&
		crrSeq = unsignSigProps()->completeRevocationRefs();

	if (crrSeq.present()) {
		xades111::CompleteRevocationRefsType::OCSPRefsOptional
					ocspRefsOpt = crrSeq.get().oCSPRefs();
		if (ocspRefsOpt.present()) {
			xades111::OCSPRefsType::OCSPRefSequence
					ocspRefSeq = ocspRefsOpt->oCSPRef();

			if (!ocspRefSeq.empty()) {
				xades111::OCSPRefType::DigestAlgAndValueOptional
				digestOpt = ocspRefSeq[0].digestAlgAndValue();

				if (digestOpt.present()) {
					dsig::DigestValueType
						digestValue = digestOpt->
								digestValue();

					data.resize(digestValue.size());
					std::copy(digestValue.data(),
						digestValue.data() +
						digestValue.size(),
						data.begin());

					xml_schema::Uri uri =
						digestOpt->digestMethod().
								algorithm();
					digestMethodUri = uri;

					return;
				}
			}
		}
	}

	THROW_STACK_EXCEPTION(
		"Missing UnsignedProperties/UnsignedSignatureProperties/Comple"
		"teRevocationRefs/OCSPRefs/OCSPRef/DigestAlgAndValue element");
}

void bdoc::XAdES111Signature::checkKeyInfo() const
{
	X509Cert x509 = getSigningCertificate();

	dsig::SignatureType::ObjectSequence const& objs = _sign->object();

	if (objs.size() != 1) {
		THROW_STACK_EXCEPTION(
			"Number of Objects is %d, must be 1", objs.size());
	}

	dsig::ObjectType::QualifyingProperties1Sequence const&
		qProps = objs[0].qualifyingProperties1();

	if (qProps.size() != 1) {
		THROW_STACK_EXCEPTION(
			"Number of QualifyingProperties is %d, must be 1",
			qProps.size());
	}

	xades111::QualifyingPropertiesType::SignedPropertiesOptional const&
		sigProps = qProps[0].signedProperties();

	if (!sigProps.present()) {
		THROW_STACK_EXCEPTION("SignedProperties not found");
	}

	xades111::CertIDListType::CertSequence const& certs =
			sigProps->signedSignatureProperties().
							signingCertificate().cert();

	if (certs.size() != 1) {
		THROW_STACK_EXCEPTION(
			"Number of SigningCertificates is %d, must be 1",
			certs.size());
	}

	dsig::DigestMethodType::AlgorithmType const&
		certDigestMethodAlgorithm =
			certs[0].certDigest().digestMethod().algorithm();

	if (!Digest::isSupported(certDigestMethodAlgorithm)) {
		THROW_STACK_EXCEPTION(
			"Unsupported digest algorithm  %s for signing "
			"certificate", certDigestMethodAlgorithm.c_str());
	}

	dsig::X509IssuerSerialType::X509IssuerNameType
		certIssuerName = certs[0].issuerSerial().x509IssuerName();

	dsig::X509IssuerSerialType::X509SerialNumberType
		certSerialNumber = certs[0].issuerSerial().x509SerialNumber();

	if (x509.compareIssuerToString(certIssuerName) ||
		x509.getSerial() != certSerialNumber) {
		THROW_STACK_EXCEPTION(
			"Signing certificate issuer information invalid");
	}

	xades111::DigestAlgAndValueType::DigestValueType const&
		certDigestValue = certs[0].certDigest().digestValue();

	std::auto_ptr<Digest> certDigestCalc =
		Digest::create(certDigestMethodAlgorithm);

	std::vector<unsigned char> derEncodedX509 = x509.encodeDER();
	certDigestCalc->update(&derEncodedX509[0], derEncodedX509.size());
	std::vector<unsigned char> calcDigest = certDigestCalc->getDigest();

	if (certDigestValue.size() != (certDigestCalc->getSize())) {
		THROW_STACK_EXCEPTION(
			"Wrong length for signing certificate digest");
	}

	for (size_t i = 0; i < certDigestCalc->getSize(); ++i) {
		if (calcDigest[i] != static_cast<unsigned char>
				(certDigestValue.data()[i])) {
			THROW_STACK_EXCEPTION(
				"Signing certificate digest does not match");
		}
	}
}

void bdoc::XAdES111Signature::checkSignedSignatureProperties() const
{
	dsig::SignatureType::ObjectSequence& os = _sign->object();
	if (os.empty()) {
		THROW_STACK_EXCEPTION("Signature block 'Object' is missing.");
	}
	else if (os.size() != 1) {
		THROW_STACK_EXCEPTION(
			"Signature block contains more than one "
			"'Object' block.");
	}
	dsig::ObjectType& o = os[0];

	dsig::ObjectType::QualifyingProperties1Sequence&
		qpSeq = o.qualifyingProperties1();
	if (qpSeq.empty()) {
		THROW_STACK_EXCEPTION(
			"Signature block 'QualifyingProperties' is missing.");
	}
	else if (qpSeq.size() != 1) {
		THROW_STACK_EXCEPTION(
			"Signature block 'Object' contains more than one "
			"'QualifyingProperties' block.");
	}
	xades111::QualifyingPropertiesType& qp = qpSeq[0];

	xades111::QualifyingPropertiesType::SignedPropertiesOptional&
		signedPropsOptional = qp.signedProperties();

	if (!signedPropsOptional.present()) {
		THROW_STACK_EXCEPTION(
			"QualifyingProperties block 'SignedProperties' "
			"is missing.");
	}
	xades111::SignedPropertiesType& signedProps = qp.signedProperties().get();

	xades111::SignedSignaturePropertiesType& signedSigProps =
		signedProps.signedSignatureProperties();

	xades111::SignedSignaturePropertiesType::
		SignaturePolicyIdentifierType
			policyOpt = signedSigProps.signaturePolicyIdentifier();
}


void bdoc::XAdES111Signature::checkQualifyingProperties() const
{
	dsig::ObjectType::QualifyingProperties1Sequence const&
		qProps = _sign->object()[0].qualifyingProperties1();

	if (qProps.size() != 1) {
		THROW_STACK_EXCEPTION(
			"Number of QualifyingProperties is %d, must be 1",
			qProps.size());
	}

	if (qProps[0].target() != "#" + _sign->id().get()) {
		THROW_STACK_EXCEPTION(
			"QualifyingProperties target is not Signature");
	}

	checkSignedSignatureProperties();

	if (qProps[0].unsignedProperties().present()) {
		xades111::QualifyingPropertiesType::UnsignedPropertiesType
			uProps = qProps[0].unsignedProperties().get();
		if (uProps.unsignedDataObjectProperties().present()) {
			THROW_STACK_EXCEPTION(
				"unexpected UnsignedDataObjectProperties in "
				"Signature");
		}
	}
}




/*
 *
 * XAdES132Signature
 *
 * */


bdoc::XAdES132Signature::XAdES132Signature(
					dsig::SignatureType* signature,
					const char *xml, size_t xml_len,
					bdoc::ContainerInfo *bdoc) : bdoc::Signature(signature, xml, xml_len, bdoc)
{
}

bdoc::XAdES132Signature::~XAdES132Signature()
{
}

const std::string& bdoc::XAdES132Signature::xadesnamespace()
{
	return XADES132_NAMESPACE;
}

bdoc::xades132::UnsignedPropertiesType::UnsignedSignaturePropertiesOptional&
bdoc::XAdES132Signature::unsignSigProps() const
{
	if (!_sign->object()[0].qualifyingProperties()[0].
					unsignedProperties().present()) {
		THROW_STACK_EXCEPTION("Missing UnsignedProperties");
	}

	return _sign->object()[0].qualifyingProperties()[0].
				unsignedProperties()->
						unsignedSignatureProperties();
}

void bdoc::XAdES132Signature::getOCSPResponseValue(std::vector<unsigned char>& data) const
{
	if (!unsignSigProps().present()) {
		THROW_STACK_EXCEPTION("Unsigned signature properties missing");
	}

	if (unsignSigProps()->revocationValues().empty()) {
		THROW_STACK_EXCEPTION("Revocation values missing");
	}

	xades132::RevocationValuesType t = unsignSigProps()->revocationValues()[0];

	xades132::OCSPValuesType tt = t.oCSPValues().get();
	xades132::OCSPValuesType::EncapsulatedOCSPValueType resp = tt.encapsulatedOCSPValue()[0];

	data.resize(resp.size());
	std::copy(resp.data(), resp.data()+resp.size(), data.begin());
}

std::string bdoc::XAdES132Signature::getProducedAt() const
{
	if (unsignSigProps().present()) {
		const xades132::OCSPIdentifierType::ProducedAtType &producedAt =
			unsignSigProps()->completeRevocationRefs()[0].oCSPRefs()->oCSPRef()[0].oCSPIdentifier().producedAt();
		return util::date::xsd2string(producedAt);
	}
	return "";
}


xml_schema::Uri bdoc::XAdES132Signature::ocspDigestAlgorithm() const
{
	return unsignSigProps()->
			completeRevocationRefs()[0].oCSPRefs()->
				oCSPRef()[0].digestAlgAndValue()->
					digestMethod().algorithm();
}

void bdoc::XAdES132Signature::getRevocationOCSPRef(
	std::vector<unsigned char>& data, std::string& digestMethodUri) const
{
	xades132::UnsignedSignaturePropertiesType::CompleteRevocationRefsSequence
		crrSeq = unsignSigProps()->completeRevocationRefs();

	if (!crrSeq.empty()) {
		xades132::CompleteRevocationRefsType::OCSPRefsOptional
					ocspRefsOpt = crrSeq[0].oCSPRefs();
		if (ocspRefsOpt.present()) {
			xades132::OCSPRefsType::OCSPRefSequence
					ocspRefSeq = ocspRefsOpt->oCSPRef();

			if (!ocspRefSeq.empty()) {
				xades132::OCSPRefType::DigestAlgAndValueOptional
				digestOpt = ocspRefSeq[0].digestAlgAndValue();

				if (digestOpt.present()) {
					dsig::DigestValueType
						digestValue = digestOpt->
								digestValue();

					data.resize(digestValue.size());
					std::copy(digestValue.data(),
						digestValue.data() +
						digestValue.size(),
						data.begin());

					xml_schema::Uri uri =
						digestOpt->digestMethod().
								algorithm();
					digestMethodUri = uri;

					return;
				}
			}
		}
	}

	THROW_STACK_EXCEPTION(
		"Missing UnsignedProperties/UnsignedSignatureProperties/Comple"
		"teRevocationRefs/OCSPRefs/OCSPRef/DigestAlgAndValue element");
}

void bdoc::XAdES132Signature::checkKeyInfo() const
{
	X509Cert x509 = getSigningCertificate();

	dsig::SignatureType::ObjectSequence const& objs = _sign->object();

	if (objs.size() != 1) {
		THROW_STACK_EXCEPTION(
			"Number of Objects is %d, must be 1", objs.size());
	}

	dsig::ObjectType::QualifyingPropertiesSequence const&
		qProps = objs[0].qualifyingProperties();

	if (qProps.size() != 1) {
		THROW_STACK_EXCEPTION(
			"Number of QualifyingProperties is %d, must be 1",
			qProps.size());
	}

	xades132::QualifyingPropertiesType::SignedPropertiesOptional const&
		sigProps =  qProps[0].signedProperties();

	if (!sigProps.present()) {
		THROW_STACK_EXCEPTION("SignedProperties not found");
	}

	xades132::SignedSignaturePropertiesType::SigningCertificateOptional const&
		sigCertOpt =
			sigProps->signedSignatureProperties().
							signingCertificate();

	if (!sigCertOpt.present()) {
		THROW_STACK_EXCEPTION("SigningCertificate not found");
	}

	xades132::CertIDListType::CertSequence const& certs = sigCertOpt->cert();

	if (certs.size() != 1) {
		THROW_STACK_EXCEPTION(
			"Number of SigningCertificates is %d, must be 1",
			certs.size());
	}

	dsig::DigestMethodType::AlgorithmType const&
		certDigestMethodAlgorithm =
			certs[0].certDigest().digestMethod().algorithm();

	if (!Digest::isSupported(certDigestMethodAlgorithm)) {
		THROW_STACK_EXCEPTION(
			"Unsupported digest algorithm  %s for signing "
			"certificate", certDigestMethodAlgorithm.c_str());
	}

	dsig::X509IssuerSerialType::X509IssuerNameType
		certIssuerName = certs[0].issuerSerial().x509IssuerName();

	dsig::X509IssuerSerialType::X509SerialNumberType
		certSerialNumber = certs[0].issuerSerial().x509SerialNumber();

	if (x509.compareIssuerToString(certIssuerName) ||
		x509.getSerial() != certSerialNumber) {
		THROW_STACK_EXCEPTION(
			"Signing certificate issuer information invalid");
	}

	xades132::DigestAlgAndValueType::DigestValueType const&
		certDigestValue = certs[0].certDigest().digestValue();

	std::auto_ptr<Digest> certDigestCalc =
		Digest::create(certDigestMethodAlgorithm);

	std::vector<unsigned char> derEncodedX509 = x509.encodeDER();
	certDigestCalc->update(&derEncodedX509[0], derEncodedX509.size());
	std::vector<unsigned char> calcDigest = certDigestCalc->getDigest();

	if (certDigestValue.size() != (certDigestCalc->getSize())) {
		THROW_STACK_EXCEPTION(
			"Wrong length for signing certificate digest");
	}

	for (size_t i = 0; i < certDigestCalc->getSize(); ++i) {
		if (calcDigest[i] != static_cast<unsigned char>
				(certDigestValue.data()[i])) {
			THROW_STACK_EXCEPTION(
				"Signing certificate digest does not match");
		}
	}
}

void bdoc::XAdES132Signature::checkSignedSignatureProperties() const
{
	dsig::SignatureType::ObjectSequence& os = _sign->object();
	if (os.empty()) {
		THROW_STACK_EXCEPTION("Signature block 'Object' is missing.");
	}
	else if (os.size() != 1) {
		THROW_STACK_EXCEPTION(
			"Signature block contains more than one "
			"'Object' block.");
	}
	dsig::ObjectType& o = os[0];

	dsig::ObjectType::QualifyingPropertiesSequence&
		qpSeq = o.qualifyingProperties();
	if (qpSeq.empty()) {
		THROW_STACK_EXCEPTION(
			"Signature block 'QualifyingProperties' is missing.");
	}
	else if (qpSeq.size() != 1) {
		THROW_STACK_EXCEPTION(
			"Signature block 'Object' contains more than one "
			"'QualifyingProperties' block.");
	}
	xades132::QualifyingPropertiesType& qp = qpSeq[0];

	xades132::QualifyingPropertiesType::SignedPropertiesOptional&
		signedPropsOptional = qp.signedProperties();

	if (!signedPropsOptional.present()) {
		THROW_STACK_EXCEPTION(
			"QualifyingProperties block 'SignedProperties' "
			"is missing.");
	}
	xades132::SignedPropertiesType& signedProps = qp.signedProperties().get();

	xades132::SignedSignaturePropertiesType& signedSigProps =
		signedProps.signedSignatureProperties();

	xades132::SignedSignaturePropertiesType::
		SignaturePolicyIdentifierOptional
			policyOpt = signedSigProps.signaturePolicyIdentifier();

	if (policyOpt.present()) {
		THROW_STACK_EXCEPTION("Signature policy is not valid");
	}
}

void bdoc::XAdES132Signature::checkQualifyingProperties() const
{
	dsig::ObjectType::QualifyingPropertiesSequence const&
		qProps = _sign->object()[0].qualifyingProperties();

	if (qProps.size() != 1) {
		THROW_STACK_EXCEPTION(
			"Number of QualifyingProperties is %d, must be 1",
			qProps.size());
	}

	if (qProps[0].target() != "#" + _sign->id().get()) {
		THROW_STACK_EXCEPTION(
			"QualifyingProperties target is not Signature");
	}

	checkSignedSignatureProperties();

	if (qProps[0].unsignedProperties().present()) {
		xades132::QualifyingPropertiesType::UnsignedPropertiesType
			uProps = qProps[0].unsignedProperties().get();
		if (uProps.unsignedDataObjectProperties().present()) {
			THROW_STACK_EXCEPTION(
				"unexpected UnsignedDataObjectProperties in "
				"Signature");
		}
	}
}

