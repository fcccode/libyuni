
#include "../../yuni.h"
#include "../../core/string.h"
#include "../../core/noncopyable.h"
#include "../adapter/sqlite.h"
#include "../../private/dbi/adapter/sqlite/sqlite3.h"
#include <string.h>
#include <cassert>


namespace Yuni
{
namespace DBI
{
namespace Adapter
{

	class ColumnInfo final
	{
	public:
		typedef std::vector<ColumnInfo> Vector;

		//! Column type
		int type;
		//! Data size
		uint size;
	};



	class SQLiteQuery final : private NonCopyable<SQLiteQuery>
	{
	public:
		explicit SQLiteQuery(::sqlite3_stmt* statement) :
			statement(statement),
			rowIndex(0),
			refcount(1), // already acquired
			columns(nullptr)
		{
			assert(statement != NULL);
		}

		~SQLiteQuery()
		{
			::sqlite3_finalize(statement);
			delete[] columns;
		}

		inline int next()
		{
			++rowIndex;
			return ::sqlite3_step(statement);
		}

		int move(uint64 index);

		int previous();

		void analyzeResultSet();

	public:
		//! SQLite statement
		sqlite3_stmt *statement;
		//! Row index
		uint64 rowIndex;
		//! Reference count
		uint refcount;
		//! Informations about all columns
		ColumnInfo* columns;
		uint columnCount;

	}; // class SQLiteQuery


	inline void SQLiteQuery::analyzeResultSet()
	{
		columnCount = (uint) sqlite3_column_count(statement);

		// resize
		columns = new ColumnInfo[columnCount];

		for (uint i = 0; i != columnCount; ++i)
		{
			int type = ::sqlite3_column_type(statement, (int) i);

			columns[i].type = type;
			columns[i].size =(type == SQLITE_BLOB or type == SQLITE_TEXT)
				? (uint) ::sqlite3_column_bytes(statement, (int) i)
				: 0u;
		}
	}


	inline int SQLiteQuery::previous()
	{
		int error = ::sqlite3_reset(statement);

		if (error == SQLITE_OK)
		{
			if (rowIndex != 0)
			{
				--rowIndex;
				for (uint64 i = 0; i != rowIndex; ++i)
				{
					error = ::sqlite3_step(statement);
					if (error != SQLITE_ROW)
					{
						if (error == SQLITE_DONE)
						{
							rowIndex = i;
							break;
						}
						else
							return error;
					}
				}
			}
		}
		return error;
	}


	inline int SQLiteQuery::move(uint64 index)
	{
		int error = ::sqlite3_reset(statement);

		if (error == SQLITE_OK)
		{
			rowIndex = 0;
			do
			{
				error = ::sqlite3_step(statement);
				if (error != SQLITE_ROW)
				{
					if (error == SQLITE_DONE)
					{
						break;
					}
					else
						return error;
				}
			}
			while (rowIndex++ < index);
		}
		return error;
	}






	static inline yn_dbierr ynsqliteError(int error, void* dbh = nullptr)
	{
		if (SQLITE_OK == error or SQLITE_DONE == error)
			return yerr_dbi_none;

		yn_dbierr e = yerr_dbi_failed;
		switch (error)
		{
			case SQLITE_CORRUPT:
				e = yerr_dbi_corrupt;
				break;
		}

		if (dbh)
		{
			const char* msg = ::sqlite3_errmsg((::sqlite3*) dbh);
			std::cerr << "SQLITE error (" << error << "): " << msg << std::endl;
		}
		else
		{
			const char* msg = ::sqlite3_errstr(error);
			std::cerr << "SQLITE error (" << error << "): " << msg << std::endl;
		}

		return e;
	}



	static yn_dbierr ynsqliteDirectQuery(void* dbh, const char* stmt, uint length)
	{
		assert(dbh != NULL);
		if (YUNI_UNLIKELY(length == 0))
			return yerr_dbi_none;

		//std::cout << "debug: sqlite exec: " << AnyString(stmt, length) << std::endl;

		sqlite3_stmt* cursor = nullptr;
		int error = ::sqlite3_prepare_v2((::sqlite3*) dbh, stmt, (int)length, &cursor, nullptr);

		if (YUNI_LIKELY(error == SQLITE_OK))
		{
			error = ::sqlite3_step(cursor);
			if (YUNI_LIKELY(error == SQLITE_ROW or error == SQLITE_DONE))
			{
				error = yerr_dbi_none;
				::sqlite3_finalize(cursor);
				return yerr_dbi_none;
			}
		}

		::sqlite3_finalize(cursor);
		return ynsqliteError(error, dbh);
	}


	static inline yn_dbierr QueryExecute(void* dbh, const AnyString& stmt)
	{
		return ynsqliteDirectQuery(dbh, stmt.c_str(), stmt.size());
	}




	static yn_dbierr ynsqliteOpen(void** dbh, const char* host, uint /*port*/, const char* /*user*/, const char* /*pass*/, const char* /*dbname*/)
	{
		assert(NULL != dbh);

		enum
		{
			flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_SHAREDCACHE | SQLITE_OPEN_URI,
		};

		::sqlite3* handle = nullptr;
		int error = ::sqlite3_open_v2(host, &handle, flags, nullptr);

		if (YUNI_UNLIKELY(SQLITE_OK != error))
		{
			// reset variables
			*dbh = nullptr;

			// error management
			switch (error)
			{
				case SQLITE_CORRUPT:
					return yerr_dbi_corrupt;
				case SQLITE_PERM:
				case SQLITE_READONLY:
					break;
			}
			return yerr_dbi_connection_failed;
		}

		*dbh = handle;


		yn_dbierr err = QueryExecute(*dbh, "PRAGMA encoding = \"UTF-8\";");
		if (YUNI_UNLIKELY(err != yerr_dbi_none))
			return err;

		return QueryExecute(*dbh, "PRAGMA foreign_keys = true;");
	}


	static void ynsqliteClose(void* dbh)
	{
		if (YUNI_LIKELY(dbh))
			ynsqliteError(::sqlite3_close_v2((::sqlite3*) dbh));
	}




	static yn_dbierr ynsqliteVacuum(void* dbh)
	{
		assert(dbh != NULL);
		return QueryExecute(dbh, "VACUUM");
	}


	static yn_dbierr ynsqliteTruncate(void* dbh, const char* tablename, uint length)
	{
		assert(dbh != NULL);

		// First, delete all rows from the table
		ShortString512 query("DELETE FROM ");
		query.append(tablename, length);
		yn_dbierr error = QueryExecute(dbh, query);
		if (error != yerr_dbi_none)
			return error;

		// secondly, reseting the autoincrement
		// note: the table name must have been checked
		//
		// note: The table `sqlite_sequence` may not exist (if there is no autoincrement)
		//
		AnyString select("SELECT name FROM sqlite_master WHERE type='table' AND name='sqlite_sequence'");

		sqlite3_stmt* cursor = nullptr;
		bool hasRow =
			(SQLITE_OK == ::sqlite3_prepare_v2((::sqlite3*) dbh, select.c_str(), (int)select.size(), &cursor, nullptr)
			and (SQLITE_ROW == ::sqlite3_step(cursor)));
		::sqlite3_finalize(cursor);

		if (hasRow)
		{
			// the table `sqlite_sequence` exists, removing any reference to our table
			query.clear();
			query.append("DELETE FROM sqlite_sequence WHERE name = '");
			query.append(tablename, length);
			query.append('\'');
			return QueryExecute(dbh, query);
		}
		else
		{
			// The table does not exist. Exiting now.
			return yerr_dbi_none;
		}
	}


	static yn_dbierr ynsqliteBegin(void* dbh)
	{
		assert(dbh != NULL);
		return QueryExecute(dbh, "BEGIN");
	}

	static yn_dbierr ynsqliteCommit(void* dbh)
	{
		assert(dbh != NULL);
		return QueryExecute(dbh, "COMMIT");
	}

	static yn_dbierr ynsqliteRollback(void* dbh)
	{
		assert(dbh != NULL);
		return QueryExecute(dbh, "ROLLBACK");
	}


	static yn_dbierr ynsqliteSavepoint(void* dbh, const char* name, uint length)
	{
		ShortString512 stmt("SAVEPOINT ");
		stmt.append(name, length);
		return QueryExecute(dbh, stmt);
	}


	static yn_dbierr ynsqliteRollbackSavepoint(void* dbh, const char* name, uint length)
	{
		ShortString512 stmt("ROLLBACK TO SAVEPOINT ");
		stmt.append(name, length);
		return QueryExecute(dbh, stmt);
	}


	static yn_dbierr ynsqliteCommitSavepoint(void* dbh, const char* name, uint length)
	{
		ShortString512 stmt("RELEASE SAVEPOINT ");
		stmt.append(name, length);
		return QueryExecute(dbh, stmt);
	}



	static yn_dbierr ynsqliteQueryNew(void** qh, void* dbh, const char* stmt, uint length)
	{
		assert(dbh != NULL);
		assert(qh != NULL);

		sqlite3_stmt* cursor;
		int error = ::sqlite3_prepare_v2((::sqlite3*) dbh, stmt, (int)length, &cursor, nullptr);

		if (YUNI_LIKELY(error == SQLITE_OK))
		{
			*qh = (void*) new SQLiteQuery(cursor);
			return yerr_dbi_none;
		}
		else
		{
			*qh = nullptr;
			return ynsqliteError(error, dbh);
		}
	}


	static void ynsqliteQueryRefAcquire(void* qh)
	{
		assert(qh != NULL);
		++ (((SQLiteQuery*) qh)->refcount);
	}


	static void ynsqliteQueryRefRelease(void* qh)
	{
		assert(qh != NULL);
		assert(((SQLiteQuery*) qh)->refcount > 0 and "invalid reference count");

		if (0 == -- (((SQLiteQuery*) qh)->refcount))
			delete ((SQLiteQuery*) qh);
	}


	static yn_dbierr ynsqliteQueryExecute(void* /*qh*/)
	{
		return yerr_dbi_none;
	}

	static yn_dbierr ynsqliteQueryPerformAndRelease(void* qh)
	{
		assert(qh != NULL);

		int error = ::sqlite3_step(((SQLiteQuery*) qh)->statement);
		yn_dbierr result = (error == SQLITE_ROW or error == SQLITE_DONE)
			? yerr_dbi_none : ynsqliteError(error);

		// release
		if (0 == -- (((SQLiteQuery*) qh)->refcount))
			delete ((SQLiteQuery*) qh);

		return result;
	}


	static yn_dbierr ynsqliteBindStr(void* qh, uint index, const char* str, uint length)
	{
		assert(qh != NULL);
		int error = ::sqlite3_bind_text(((SQLiteQuery*) qh)->statement, (int)(index + 1), str, (int)length, 0);
		return ynsqliteError(error);
	}


	static yn_dbierr ynsqliteBindInt32(void* qh, uint index, yint32 value)
	{
		assert(qh != NULL);
		int error = ::sqlite3_bind_int(((SQLiteQuery*) qh)->statement, (int)(index + 1), value);
		return ynsqliteError(error);
	}


	static yn_dbierr ynsqliteBindInt64(void* qh, uint index, yint64 value)
	{
		assert(qh != NULL);
		int error = ::sqlite3_bind_int64(((SQLiteQuery*) qh)->statement, (int)(index + 1), value);
		return ynsqliteError(error);
	}


	static yn_dbierr ynsqliteBindBool(void* qh, uint index, int value)
	{
		assert(qh != NULL);
		int error = ::sqlite3_bind_int(((SQLiteQuery*) qh)->statement, (int)(index + 1), (int) value);
		return ynsqliteError(error);
	}


	static yn_dbierr ynsqliteBindDouble(void* qh, uint index, double value)
	{
		assert(qh != NULL);
		int error = ::sqlite3_bind_double(((SQLiteQuery*) qh)->statement, (int)(index + 1), value);
		return ynsqliteError(error);
	}


	static yn_dbierr ynsqliteGoToNext(void* qh)
	{
		assert(qh != NULL);
		int error = ((SQLiteQuery*) qh)->next();

		switch (error)
		{
			case SQLITE_ROW:
				return yerr_dbi_none;
			case SQLITE_DONE:
				return yerr_dbi_no_row;
			default:
				// another error has occured
				return ynsqliteError(error);
		}
	}


	static yn_dbierr ynsqliteGoToPrevious(void* qh)
	{
		assert(qh != NULL);
		int error = ((SQLiteQuery*) qh)->previous();

		if (error == SQLITE_ROW)
			return yerr_dbi_none;
		if (error == SQLITE_DONE)
			return yerr_dbi_no_row;

		// another error has occured
		return ynsqliteError(error);
	}


	static yn_dbierr ynsqliteGoTo(void* qh, yuint64 rowindex)
	{
		int error = ((SQLiteQuery*) qh)->move(rowindex);

		if (error == SQLITE_ROW)
			return yerr_dbi_none;
		if (error == SQLITE_DONE)
			return yerr_dbi_no_row;

		// another error has occured
		return ynsqliteError(error);
	}


	static sint32 ynsqliteColumnToInt32(void* qh, uint colindex)
	{
		assert(qh != NULL);
		assert(colindex < 2147483640);
		return ::sqlite3_column_int(((SQLiteQuery*) qh)->statement, (int)colindex);
	}


	static sint64 ynsqliteColumnToInt64(void* qh, uint colindex)
	{
		assert(qh != NULL);
		assert(colindex < 2147483640);
		return ::sqlite3_column_int64(((SQLiteQuery*) qh)->statement, (int)colindex);
	}


	static double ynsqliteColumnToDouble(void* qh, uint colindex)
	{
		assert(qh != NULL);
		assert(colindex < 2147483640);
		return ::sqlite3_column_double(((SQLiteQuery*) qh)->statement, (int)colindex);
	}


	static const char* ynsqliteColumnToString(void* qh, uint colindex, uint* length)
	{
		assert(qh != NULL);
		assert(colindex < 2147483640);
		assert(length != NULL);

		sqlite3_stmt* stmt = ((SQLiteQuery*) qh)->statement;
		int size = ::sqlite3_column_bytes(stmt, (int) colindex);

		if (size > 0)
		{
			*length = (uint) size;
			return (const char*) sqlite3_column_text(stmt, (int) colindex);
		}
		else
		{
			*length = 0;
			return nullptr;
		}
	}


	static int ynsqliteColumnIsNull(void* qh, uint colindex)
	{
		assert(qh != NULL);
		assert(colindex < 2147483640);

		return (SQLITE_NULL == ::sqlite3_column_type(((SQLiteQuery*) qh)->statement, (int)colindex));
	}






	void SQLite::retrieveEntries(::yn_dbi_adapter& entries)
	{
		// database
		entries.open         = & ynsqliteOpen;
		entries.close        = & ynsqliteClose;
		entries.open_schema  = nullptr;

		// misc
		entries.vacuum       = & ynsqliteVacuum;
		entries.truncate     = & ynsqliteTruncate;

		// transactions
		entries.begin                = & ynsqliteBegin;
		entries.commit               = & ynsqliteCommit;
		entries.rollback             = & ynsqliteRollback;
		entries.rollback_savepoint   = & ynsqliteRollbackSavepoint;
		entries.savepoint            = & ynsqliteSavepoint;
		entries.commit_savepoint     = & ynsqliteCommitSavepoint;

		// queries
		entries.query_exec                = & ynsqliteDirectQuery;
		entries.query_new                 = & ynsqliteQueryNew;
		entries.query_ref_acquire         = & ynsqliteQueryRefAcquire;
		entries.query_ref_release         = & ynsqliteQueryRefRelease;
		entries.query_execute             = & ynsqliteQueryExecute;
		entries.query_perform_and_release = & ynsqliteQueryPerformAndRelease;
		entries.bind_str                  = & ynsqliteBindStr;
		entries.bind_bool                 = & ynsqliteBindBool;
		entries.bind_int32                = & ynsqliteBindInt32;
		entries.bind_int64                = & ynsqliteBindInt64;
		entries.bind_double               = & ynsqliteBindDouble;

		// cursor
		entries.cursor_go_to_next      = & ynsqliteGoToNext;
		entries.cursor_go_to_previous  = & ynsqliteGoToPrevious;
		entries.cursor_go_to           = & ynsqliteGoTo;

		// columns
		entries.column_to_int32      = & ynsqliteColumnToInt32;
		entries.column_to_int64      = & ynsqliteColumnToInt64;
		entries.column_to_double     = & ynsqliteColumnToDouble;
		entries.column_to_cstring    = & ynsqliteColumnToString;
		entries.column_is_null       = & ynsqliteColumnIsNull;
	}





} // namespace Adapter
} // namespace DBI
} // namespace Yuni

