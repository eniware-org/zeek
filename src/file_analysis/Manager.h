// See the file "COPYING" in the main distribution directory for copyright.

#ifndef FILE_ANALYSIS_MANAGER_H
#define FILE_ANALYSIS_MANAGER_H

#include <string>
#include <queue>

#include "Dict.h"
#include "Net.h"
#include "Conn.h"
#include "Val.h"
#include "Analyzer.h"
#include "Timer.h"
#include "EventHandler.h"
#include "RuleMatcher.h"

#include "File.h"
#include "FileTimer.h"
#include "Component.h"
#include "Tag.h"
#include "plugin/ComponentManager.h"
#include "analyzer/Tag.h"

#include "file_analysis/file_analysis.bif.h"

namespace file_analysis {

/**
 * Main entry point for interacting with file analysis.
 */
class Manager : public plugin::ComponentManager<Tag, Component> {
public:

	/**
	 * Constructor.
	 */
	Manager();

	/**
	 * Destructor.  Times out any currently active file analyses.
	 */
	~Manager();

	/**
	 * First-stage initializion of the manager. This is called early on
	 * during Bro's initialization, before any scripts are processed.
	 */
	void InitPreScript();

	/**
	 * Second-stage initialization of the manager. This is called late
	 * during Bro's initialization after any scripts are processed.
	 */
	void InitPostScript();

	/**
	 * Initializes the state required to match against file magic signatures
	 * for MIME type identification.
	 */
	void InitMagic();

	/**
	 * Times out any active file analysis to prepare for shutdown.
	 */
	void Terminate();

	/**
	 * Creates a file identifier from a unique file handle string.
	 * @param handle a unique string (may contain NULs) which identifies
	 * a single file.
	 * @return a prettified MD5 hash of \a handle, truncated to *bits_per_uid* bits.
	 */
	string HashHandle(const string& handle) const;

	/**
	 * Take in a unique file handle string to identify next piece of
	 * incoming file data/information.
	 * @param handle a unique string (may contain NULs) which identifies
	 * a single file.
	 */
	void SetHandle(const string& handle);

	/**
	 * Pass in non-sequential file data.
	 * @param data pointer to start of a chunk of file data.
	 * @param len number of bytes in the data chunk.
	 * @param offset number of bytes from start of file that data chunk occurs.
	 * @param tag network protocol over which the file data is transferred.
	 * @param conn network connection over which the file data is transferred.
	 * @param is_orig true if the file is being sent from connection originator
	 *        or false if is being sent in the opposite direction.
	 * @param precomputed_file_id may be set to a previous return value in order to
	 *        bypass costly file handle lookups.
	 * @param mime_type may be set to the mime type of the file, if already known due
	 *        to the protocol. This is, e.g., the case in TLS connections where X.509
	 *        certificates are passed as files; here the type of the file is set by
	 *        the protocol. If this parameter is given, MIME type detection will be
	 *        disabled.
	 *        This parameter only has any effect for the first DataIn call of each
	 *        file. It is ignored for all subsequent calls.
	 * @return a unique file ID string which, in certain contexts, may be
	 *         cached and passed back in to a subsequent function call in order
	 *         to avoid costly file handle lookups (which have to go through
	 *         the \c get_file_handle script-layer event).  An empty string
	 *         indicates the associate file is not going to be analyzed further.
	 */
	std::string DataIn(const u_char* data, uint64 len, uint64 offset,
	                   analyzer::Tag tag, Connection* conn, bool is_orig,
	                   const std::string& precomputed_file_id = "",
	                   const std::string& mime_type = "");

	/**
	 * Pass in sequential file data.
	 * @param data pointer to start of a chunk of file data.
	 * @param len number of bytes in the data chunk.
	 * @param tag network protocol over which the file data is transferred.
	 * @param conn network connection over which the file data is transferred.
	 * @param is_orig true if the file is being sent from connection originator
	 *        or false if is being sent in the opposite direction.
	 * @param precomputed_file_id may be set to a previous return value in order to
	 *        bypass costly file handle lookups.
	 * @param mime_type may be set to the mime type of the file, if already known due
	 *        to the protocol. This is, e.g., the case in TLS connections where X.509
	 *        certificates are passed as files; here the type of the file is set by
	 *        the protocol. If this parameter is give, mime type detection will be
	 *        disabled.
	 *        This parameter is only used for the first bit of data for each file.
	 * @return a unique file ID string which, in certain contexts, may be
	 *         cached and passed back in to a subsequent function call in order
	 *         to avoid costly file handle lookups (which have to go through
	 *         the \c get_file_handle script-layer event).  An empty string
	 *         indicates the associated file is not going to be analyzed further.
	 */
	std::string DataIn(const u_char* data, uint64 len, analyzer::Tag tag,
	                   Connection* conn, bool is_orig,
	                   const std::string& precomputed_file_id = "",
	                   const std::string& mime_type = "");

	/**
	 * Pass in sequential file data from external source (e.g. input framework).
	 * @param data pointer to start of a chunk of file data.
	 * @param len number of bytes in the data chunk.
	 * @param file_id an identifier for the file (usually a hash of \a source).
	 * @param source uniquely identifies the file and should also describe
	 *        in human-readable form where the file input is coming from (e.g.
	 *        a local file path).
	 */
	void DataIn(const u_char* data, uint64 len, const string& file_id,
	            const string& source);

	/**
	 * Signal the end of file data regardless of which direction it is being
	 * sent over the connection.
	 * @param tag network protocol over which the file data is transferred.
	 * @param conn network connection over which the file data is transferred.
	 */
	void EndOfFile(analyzer::Tag tag, Connection* conn);

	/**
	 * Signal the end of file data being transferred over a connection in
	 * a particular direction.
	 * @param tag network protocol over which the file data is transferred.
	 * @param conn network connection over which the file data is transferred.
	 */
	void EndOfFile(analyzer::Tag tag, Connection* conn, bool is_orig);

	/**
	 * Signal the end of file data being transferred using the file identifier.
	 * @param file_id the file identifier/hash.
	 */
	void EndOfFile(const string& file_id);

	/**
	 * Signal a gap in the file data stream.
	 * @param offset number of bytes in to file at which missing chunk starts.
	 * @param len length in bytes of the missing chunk of file data.
	 * @param tag network protocol over which the file data is transferred.
	 * @param conn network connection over which the file data is transferred.
	 * @param is_orig true if the file is being sent from connection originator
	 *        or false if is being sent in the opposite direction.
	 * @param precomputed_file_id may be set to a previous return value in order to
	 *        bypass costly file handle lookups.
	 * @return a unique file ID string which, in certain contexts, may be
	 *         cached and passed back in to a subsequent function call in order
	 *         to avoid costly file handle lookups (which have to go through
	 *         the \c get_file_handle script-layer event).  An empty string
	 *         indicates the associate file is not going to be analyzed further.
	 */
	std::string Gap(uint64 offset, uint64 len, analyzer::Tag tag,
	                Connection* conn, bool is_orig,
	                const std::string& precomputed_file_id = "");

	/**
	 * Provide the expected number of bytes that comprise a file.
	 * @param size the number of bytes in the full file.
	 * @param tag network protocol over which the file data is transferred.
	 * @param conn network connection over which the file data is transferred.
	 * @param is_orig true if the file is being sent from connection originator
	 *        or false if is being sent in the opposite direction.
	 * @param precomputed_file_id may be set to a previous return value in order to
	 *        bypass costly file handle lookups.
	 * @return a unique file ID string which, in certain contexts, may be
	 *         cached and passed back in to a subsequent function call in order
	 *         to avoid costly file handle lookups (which have to go through
	 *         the \c get_file_handle script-layer event).  An empty string
	 *         indicates the associate file is not going to be analyzed further.
	 */
	std::string SetSize(uint64 size, analyzer::Tag tag, Connection* conn,
	                    bool is_orig, const std::string& precomputed_file_id = "");

	/**
	 * Starts ignoring a file, which will finally be removed from internal
	 * mappings on EOF or TIMEOUT.
	 * @param file_id the file identifier/hash.
	 * @return false if file identifier did not map to anything, else true.
	 */
	bool IgnoreFile(const string& file_id);

	/**
	 * Set's an inactivity threshold for the file.
	 * @param file_id the file identifier/hash.
	 * @param interval the amount of time in which no activity is seen for
	 *        the file identified by \a file_id that will cause the file
	 *        to be considered stale, timed out, and then resource reclaimed.
	 * @return false if file identifier did not map to anything, else true.
	 */
	bool SetTimeoutInterval(const string& file_id, double interval) const;

	/**
	 * Enable the reassembler for a file.
	 */
	bool EnableReassembly(const string& file_id);
	
	/**
	 * Disable the reassembler for a file.
	 */
	bool DisableReassembly(const string& file_id);

	/**
	 * Set the reassembly for a file in bytes.
	 */
	bool SetReassemblyBuffer(const string& file_id, uint64 max);

	/**
	 * Sets a limit on the maximum size allowed for extracting the file
	 * to local disk;
	 * @param file_id the file identifier/hash.
	 * @param args a \c AnalyzerArgs value which describes a file analyzer,
	 *        which should be a file extraction analyzer.
	 * @param n the new extraction limit, in bytes.
	 * @return false if file identifier and analyzer did not map to anything,
	 *         else true.
	 */
	bool SetExtractionLimit(const string& file_id, RecordVal* args,
	                        uint64 n) const;

	/**
	 * Try to retrieve a file that's being analyzed, using its identifier/hash.
	 * @param file_id the file identifier/hash.
	 * @return the File object mapped to \a file_id, or a null pointer if no
	 *         mapping exists.
	 */
	File* LookupFile(const string& file_id) const;

	/**
	 * Queue attachment of an analzer to the file identifier.  Multiple
	 * analyzers of a given type can be attached per file identifier at a time
	 * as long as the arguments differ.
	 * @param file_id the file identifier/hash.
	 * @param tag the analyzer tag of the file analyzer to add.
	 * @param args a \c AnalyzerArgs value which describes a file analyzer.
	 * @return false if the analyzer failed to be instantiated, else true.
	 */
	bool AddAnalyzer(const string& file_id, file_analysis::Tag tag,
	                 RecordVal* args) const;

	/**
	 * Queue removal of an analyzer for a given file identifier.
	 * @param file_id the file identifier/hash.
	 * @param tag the analyzer tag of the file analyzer to remove.
	 * @param args a \c AnalyzerArgs value which describes a file analyzer.
	 * @return true if the analyzer is active at the time of call, else false.
	 */
	bool RemoveAnalyzer(const string& file_id, file_analysis::Tag tag,
	                    RecordVal* args) const;

	/**
	 * Tells whether analysis for a file is active or ignored.
	 * @param file_id the file identifier/hash.
	 * @return whether the file mapped to \a file_id is being ignored.
	 */
	bool IsIgnored(const string& file_id);

	/**
	 * Instantiates a new file analyzer instance for the file.
	 * @param tag The file analyzer's tag.
	 * @param args The file analzer argument/option values.
	 * @param f The file analzer is to be associated with.
	 * @return The new analyzer instance or null if tag is invalid.
	 */
	Analyzer* InstantiateAnalyzer(Tag tag, RecordVal* args, File* f) const;

	/**
	 * Returns a set of all matching MIME magic signatures for a given
	 * chunk of data.
	 * @param data A chunk of bytes to match magic MIME signatures against.
	 * @param len The number of bytes in \a data.
	 * @param rval An optional pre-existing structure in which to insert
	 *             new matches.  If it's a null pointer, an object is
	 *             allocated and returned from the method.
	 * @return Set of all matching file magic signatures, which may be
	 *         an object allocated by the method if \a rval is a null pointer.
	 */
	RuleMatcher::MIME_Matches* DetectMIME(const u_char* data, uint64 len,
					      RuleMatcher::MIME_Matches* rval) const;

	/**
	 * Returns the strongest MIME magic signature match for a given data chunk.
	 * @param data A chunk of bytes to match magic MIME signatures against.
	 * @param len The number of bytes in \a data.
	 * @returns The MIME type string of the strongest file magic signature
	 *          match, or an empty string if nothing matched.
	 */
	std::string DetectMIME(const u_char* data, uint64 len) const;

	uint64 CurrentFiles()
		{ return id_map.Length(); }

	uint64 MaxFiles()
		{ return id_map.MaxLength(); }

	uint64 CumulativeFiles()
		{ return id_map.NumCumulativeInserts(); }

protected:
	friend class FileTimer;

	typedef PDict<bool> IDSet;
	typedef PDict<File> IDMap;

	/**
	 * Create a new file to be analyzed or retrieve an existing one.
	 * @param file_id the file identifier/hash.
	 * @param conn network connection, if any, over which the file is
	 *        transferred.
	 * @param tag network protocol, if any, over which the file is transferred.
	 * @param is_orig true if the file is being sent from connection originator
	 *        or false if is being sent in the opposite direction (or if it
	 *        this file isn't related to a connection).
	 * @param update_conn whether we need to update connection-related field
	 *        in the \c fa_file record value associated with the file.
	 * @param an optional value of the source field to fill in.
	 * @return the File object mapped to \a file_id or a null pointer if
	 *         analysis is being ignored for the associated file.  An File
	 *         object may be created if a mapping doesn't exist, and if it did
	 *         exist, the activity time is refreshed along with any
	 *         connection-related fields.
	 */
	File* GetFile(const string& file_id, Connection* conn = 0,
	              analyzer::Tag tag = analyzer::Tag::Error,
	              bool is_orig = false, bool update_conn = true,
	              const char* source_name = 0);

	/**
	 * Evaluate timeout policy for a file and remove the File object mapped to
	 * \a file_id if needed.
	 * @param file_id the file identifier/hash.
	 * @param is_termination whether the Manager (and probably Bro) is in a
	 *        terminating state.  If true, then the timeout cannot be postponed.
	 */
	void Timeout(const string& file_id, bool is_terminating = ::terminating);

	/**
	 * Immediately remove file_analysis::File object associated with \a file_id.
	 * @param file_id the file identifier/hash.
	 * @return false if file id string did not map to anything, else true.
	 */
	bool RemoveFile(const string& file_id);

	/**
	 * Sets #current_file_id to a hash of a unique file handle string based on
	 * what the \c get_file_handle event derives from the connection params.
	 * Event queue is flushed so that we can get the handle value immediately.
	 * @param tag network protocol over which the file is transferred.
	 * @param conn network connection over which the file is transferred.
	 * @param is_orig true if the file is being sent from connection originator
	 *        or false if is being sent in the opposite direction.
	 * @return #current_file_id, which is a hash of a unique file handle string
	 *         set by a \c get_file_handle event handler.
	 */
	std::string GetFileID(analyzer::Tag tag, Connection* c, bool is_orig);

	/**
	 * Check if analysis is available for files transferred over a given
	 * network protocol.
	 * @param tag the network protocol over which files can be transferred and
	 *        analyzed by the file analysis framework.
	 * @return whether file analysis is disabled for the analyzer given by
	 *         \a tag.
	 */
	static bool IsDisabled(analyzer::Tag tag);

private:
	typedef set<Tag> TagSet;
	typedef map<string, TagSet*> MIMEMap;

	TagSet* LookupMIMEType(const string& mtype, bool add_if_not_found);

	PDict<File> id_map;  /**< Map file ID to file_analysis::File records. */
	PDict<bool> ignored; /**< Ignored files.  Will be finally removed on EOF. */
	string current_file_id;	/**< Hash of what get_file_handle event sets. */
	RuleFileMagicState* magic_state;	/**< File magic signature match state. */
	MIMEMap mime_types;/**< Mapping of MIME types to analyzers. */

	static TableVal* disabled;	/**< Table of disabled analyzers. */
	static TableType* tag_set_type;	/**< Type for set[tag]. */
	static string salt; /**< A salt added to file handles before hashing. */
};

/**
 * Returns a script-layer value corresponding to the \c mime_matches type.
 * @param m The MIME match information with which to populate the value.
 */
VectorVal* GenMIMEMatchesVal(const RuleMatcher::MIME_Matches& m);

} // namespace file_analysis

extern file_analysis::Manager* file_mgr;

#endif
